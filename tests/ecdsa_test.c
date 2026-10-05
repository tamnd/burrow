/* Derived from Go's src/crypto/ecdsa/ecdsa_test.go, equal_test.go and
 * ecdsa_wycheproof_test.go, and the tests and self tests of
 * src/crypto/internal/fips140/ecdsa: ecdsa_test.go and cast.go.
 * Go source: go1.27.1.
 *
 * SigVer.rsp.bz2, det-keygen.json and the Wycheproof vectors Go fetches as a
 * module are in ecdsa_test_gen.h, which tools/gen-ecdsa-tests.sh writes, with
 * the Wycheproof keys already out of their DER as Go's x509 would have taken
 * them. There is no x509 here yet either, so TestKeyGenerationVectors and
 * TestEqual put together and pull apart the PKCS #8 encoding of a key with
 * pkcs8_marshal and pkcs8_parse below, which write and read what
 * x509.MarshalPKCS8PrivateKey and ParsePKCS8PrivateKey do for an ECDSA key.
 *
 * cast.go is the self test FIPS mode runs before the first signature. burrow
 * has no FIPS mode, so the two known answers it checks are tests here,
 * TestCASTSign and TestCASTSignDeterministic.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/bufio.h"
#include "burrow/burrow.h"
#include "burrow/bytes.h"
#include "burrow/compress/bzip2.h"
#include "burrow/crypto.h"
#include "burrow/crypto/ecdh.h"
#include "burrow/crypto/ecdsa.h"
#include "burrow/crypto/elliptic.h"
#include "burrow/crypto/rand.h"
#include "burrow/crypto/sha256.h"
#include "burrow/crypto/sha512.h"
#include "burrow/encoding/hex.h"
#include "burrow/io.h"
#include "burrow/math/big.h"
#include "burrow/mem/heap.h"
#include "burrow/slices.h"
#include "burrow/strings.h"

#include "../src/crypto/cryptobyte.h"
#include "../src/crypto/ecdsa_internal.h"
#include "../src/crypto/nistec.h"
#include "../src/crypto/rand_internal.h"

#include "ecdsa_test_gen.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static Slice bs(const void *p, Int n) {
    return slice_from((void *)(uintptr_t)p, n, n, TYPE_BYTE);
}

static Slice cs(const char *s) {
    return bs(s, (Int)strlen(s));
}

static Slice unhex(Alloc *a, const char *s) {
    Error err = BURROW_NO_ERROR;
    Slice b = hex_decode_string(a, str_from_cstr(s), &err);
    if (BURROW_FAILED(err))
        panic_str(BURROW_S("invalid hex"));
    return b;
}

static Str hexs(Alloc *a, Slice b) {
    return hex_encode_to_string(a, b);
}

/* fromHex. */
static BigInt *from_hex(Alloc *a, const char *s) {
    BigInt *r = big_new_int(a, 0);
    bool ok = false;
    big_int_set_string(r, str_from_cstr(s), 16, &ok);
    if (!ok)
        panic_str(BURROW_S("bad hex"));
    return r;
}

static Slice digest(Alloc *a, CryptoHash h, Slice msg) {
    Hash hh = crypto_hash_new(h, a);
    hash_write(hh, msg, NULL);
    return hash_sum(a, hh, (Slice){0});
}

static bool same_curve(EllipticCurve x, EllipticCurve y) {
    return x.vt == y.vt && x.data == y.data;
}

static const Type test_type = {
    {(const Byte *)"readerFunc", 10},
    {(const Byte *)"ecdsa_test", 10},
    KIND_STRUCT,
    0,
    1,
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x65636473U,
    NULL,
};

/* zeroReader, which fills whatever it is given with zeros. */
static Int zero_read(void *self, Slice p, Error *err) {
    (void)self;
    (void)err;
    if (p.len > 0)
        memset(p.p, 0, (size_t)p.len);
    return p.len;
}

static const IoReaderVT zero_vt = {&test_type, zero_read};

static IoReader zero_reader(void) {
    IoReader r = {&zero_vt, NULL};
    return r;
}

/* The four curves by the names Go's tests and vectors give them. */
static EllipticCurve curve_by_name(const char *name) {
    if (strcmp(name, "P-224") == 0 || strcmp(name, "P224") == 0 ||
        strcmp(name, "secp224r1") == 0)
        return elliptic_p224();
    if (strcmp(name, "P-256") == 0 || strcmp(name, "P256") == 0 ||
        strcmp(name, "secp256r1") == 0)
        return elliptic_p256();
    if (strcmp(name, "P-384") == 0 || strcmp(name, "P384") == 0 ||
        strcmp(name, "secp384r1") == 0)
        return elliptic_p384();
    if (strcmp(name, "P-521") == 0 || strcmp(name, "P521") == 0 ||
        strcmp(name, "secp521r1") == 0)
        return elliptic_p521();
    return (EllipticCurve){0};
}

/* --------------------------------------------------------------- curves */

typedef void (*CurveTest)(TestingT *t, EllipticCurve c, bool generic);

typedef struct CurveEnv {
    CurveTest f;
    EllipticCurve c;
    bool generic;
} CurveEnv;

static void curve_run(void *env, TestingT *t) {
    const CurveEnv *e = env;
    e->f(t, e->c, e->generic);
}

/* testAllCurves. P256/Generic is a copy of P-256's params, which goes through
 * the math/big implementation, as genericParamsForCurve has it. */
static void test_all_curves(TestingT *t, CurveTest f) {
    EllipticCurveParams generic = *elliptic_curve_params(elliptic_p256());
    struct {
        const char *name;
        EllipticCurve c;
        bool generic;
    } tests[] = {
        {"P256", elliptic_p256(), false},
        {"P224", elliptic_p224(), false},
        {"P384", elliptic_p384(), false},
        {"P521", elliptic_p521(), false},
        {"P256/Generic", elliptic_curve_params_as_elliptic_curve(&generic), true},
    };
    int n = testing_short() ? 1 : (int)(sizeof tests / sizeof tests[0]);
    for (int i = 0; i < n; i++) {
        CurveEnv e = {f, tests[i].c, tests[i].generic};
        testing_t_run(t, str_from_cstr(tests[i].name),
                      BURROW_FN(TestingTFunc, curve_run, &e));
    }
}

/* -------------------------------------------------------- KeyGeneration */

static void key_generation(TestingT *t, EllipticCurve c, bool generic) {
    (void)generic;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;

    EcdsaPrivateKey *priv = ecdsa_generate_key(a, c, crypto_rand_reader, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    if (!elliptic_curve_is_on_curve(c, priv->public_key.x, priv->public_key.y))
        testing_t_errorf_v(t, "public key invalid: %s", error_text(err));

    arena_free(&ar);
}

static void TestKeyGeneration(TestingT *t) {
    test_all_curves(t, key_generation);
}

/* -------------------------------------------------------- SignAndVerify */

static void sign_and_verify(TestingT *t, EllipticCurve c, bool generic) {
    (void)generic;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;

    EcdsaPrivateKey *priv = ecdsa_generate_key(a, c, crypto_rand_reader, &err);
    uint8_t hashed[] = "testing";
    Slice h = bs(hashed, 7);
    BigInt *s = NULL;
    BigInt *r = ecdsa_sign(a, crypto_rand_reader, priv, h, &s, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "error signing: %s", error_text(err));
        arena_free(&ar);
        return;
    }

    if (!ecdsa_verify(&priv->public_key, h, r, s))
        testing_t_errorf_v(t, "Verify failed");

    hashed[0] ^= 0xff;
    if (ecdsa_verify(&priv->public_key, h, r, s))
        testing_t_errorf_v(t, "Verify always works!");

    arena_free(&ar);
}

static void TestSignAndVerify(TestingT *t) {
    test_all_curves(t, sign_and_verify);
}

static void sign_and_verify_asn1(TestingT *t, EllipticCurve c, bool generic) {
    (void)generic;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;

    EcdsaPrivateKey *priv = ecdsa_generate_key(a, c, crypto_rand_reader, &err);
    uint8_t hashed[] = "testing";
    Slice h = bs(hashed, 7);
    Slice sig = ecdsa_sign_asn1(a, crypto_rand_reader, priv, h, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "error signing: %s", error_text(err));
        arena_free(&ar);
        return;
    }

    if (!ecdsa_verify_asn1(&priv->public_key, h, sig))
        testing_t_errorf_v(t, "VerifyASN1 failed");

    hashed[0] ^= 0xff;
    if (ecdsa_verify_asn1(&priv->public_key, h, sig))
        testing_t_errorf_v(t, "VerifyASN1 always works!");

    arena_free(&ar);
}

static void TestSignAndVerifyASN1(TestingT *t) {
    test_all_curves(t, sign_and_verify_asn1);
}

/* --------------------------------------------------- EmptyHashRejection */

/* nil and []byte{}, which are the same thing to C apart from the pointer. */
static const uint8_t empty_byte[1];
#define NIL_HASH ((Slice){0})
#define EMPTY_HASH bs(empty_byte, 0)

static void check_empty_error(TestingT *t, Error err, const char *what) {
    if (!BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s should fail", what);
    if (!strings_contains(error_text(err), BURROW_S("cannot be empty")))
        testing_t_errorf_v(t, "unexpected error: %s", error_text(err));
}

static void empty_sign_asn1(void *env, TestingT *t) {
    const EcdsaPrivateKey *priv = env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    Error err = BURROW_NO_ERROR;
    (void)ecdsa_sign_asn1(a, crypto_rand_reader, priv, NIL_HASH, &err);
    check_empty_error(t, err, "SignASN1 with nil hash");

    err = BURROW_NO_ERROR;
    (void)ecdsa_sign_asn1(a, crypto_rand_reader, priv, EMPTY_HASH, &err);
    check_empty_error(t, err, "SignASN1 with empty hash");

    arena_free(&ar);
}

static void empty_sign(void *env, TestingT *t) {
    const EcdsaPrivateKey *priv = env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    Error err = BURROW_NO_ERROR;
    (void)ecdsa_private_key_sign(priv, a, crypto_rand_reader, NIL_HASH,
                                 (CryptoSignerOpts){0}, &err);
    check_empty_error(t, err, "Sign with nil hash");

    err = BURROW_NO_ERROR;
    (void)ecdsa_private_key_sign(priv, a, crypto_rand_reader, EMPTY_HASH,
                                 (CryptoSignerOpts){0}, &err);
    check_empty_error(t, err, "Sign with empty hash");

    arena_free(&ar);
}

static void empty_sign_deterministic(void *env, TestingT *t) {
    const EcdsaPrivateKey *priv = env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    Error err = BURROW_NO_ERROR;
    (void)ecdsa_private_key_sign(priv, a, (IoReader){0}, NIL_HASH,
                                 (CryptoSignerOpts){0}, &err);
    if (!BURROW_FAILED(err))
        testing_t_errorf_v(t, "deterministic Sign with nil hash should fail");
    err = BURROW_NO_ERROR;
    (void)ecdsa_private_key_sign(priv, a, (IoReader){0}, EMPTY_HASH,
                                 (CryptoSignerOpts){0}, &err);
    if (!BURROW_FAILED(err))
        testing_t_errorf_v(t, "deterministic Sign with empty hash should fail");

    arena_free(&ar);
}

static void empty_verify_asn1(void *env, TestingT *t) {
    const EcdsaPrivateKey *priv = env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    /* Create a valid signature first. */
    Error err = BURROW_NO_ERROR;
    Slice sig = ecdsa_sign_asn1(a, crypto_rand_reader, priv, cs("test hash"), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));

    if (ecdsa_verify_asn1(&priv->public_key, NIL_HASH, sig))
        testing_t_errorf_v(t, "VerifyASN1 with nil hash should return false");
    if (ecdsa_verify_asn1(&priv->public_key, EMPTY_HASH, sig))
        testing_t_errorf_v(t, "VerifyASN1 with empty hash should return false");

    arena_free(&ar);
}

static void empty_hash_rejection(TestingT *t, EllipticCurve c, bool generic) {
    (void)generic;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;

    EcdsaPrivateKey *priv = ecdsa_generate_key(a, c, crypto_rand_reader, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));

    testing_t_run(t, BURROW_S("SignASN1"),
                  BURROW_FN(TestingTFunc, empty_sign_asn1, priv));
    testing_t_run(t, BURROW_S("Sign"), BURROW_FN(TestingTFunc, empty_sign, priv));
    testing_t_run(t, BURROW_S("SignDeterministic"),
                  BURROW_FN(TestingTFunc, empty_sign_deterministic, priv));
    testing_t_run(t, BURROW_S("VerifyASN1"),
                  BURROW_FN(TestingTFunc, empty_verify_asn1, priv));

    arena_free(&ar);
}

static void TestEmptyHashRejection(TestingT *t) {
    test_all_curves(t, empty_hash_rejection);
}

/* ------------------------------------------------------- SignHashLength */

static bool sign_fails(const EcdsaPrivateKey *priv, Alloc *a, Slice digest,
                       CryptoSignerOpts opts) {
    Error err = BURROW_NO_ERROR;
    (void)ecdsa_private_key_sign(priv, a, crypto_rand_reader, digest, opts, &err);
    return BURROW_FAILED(err);
}

static void sign_hash_length(TestingT *t, EllipticCurve c, bool generic) {
    (void)generic;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;

    EcdsaPrivateKey *priv = ecdsa_generate_key(a, c, crypto_rand_reader, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));

    Sha256Sum256Ret d = sha256_sum256(cs("message"));
    Slice dg = bs(d.a, sizeof d.a);
    CryptoHash sha256h = CRYPTO_SHA256, sha384h = CRYPTO_SHA384, zero = 0;

    /* opts == nil is allowed and skips the length check. */
    err = BURROW_NO_ERROR;
    (void)ecdsa_private_key_sign(priv, a, crypto_rand_reader, dg, (CryptoSignerOpts){0},
                                 &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Sign with nil opts: %s", error_text(err));

    /* opts != nil with matching hash length succeeds. */
    err = BURROW_NO_ERROR;
    (void)ecdsa_private_key_sign(priv, a, crypto_rand_reader, dg,
                                 crypto_hash_as_signer_opts(&sha256h), &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Sign with matching hash: %s", error_text(err));

    /* opts != nil with mismatched hash length fails. */
    if (!sign_fails(priv, a, dg, crypto_hash_as_signer_opts(&sha384h)))
        testing_t_errorf_v(t, "Sign with mismatched hash length should fail");
    if (!sign_fails(priv, a, bs(d.a, sizeof d.a - 1),
                    crypto_hash_as_signer_opts(&sha256h)))
        testing_t_errorf_v(t, "Sign with short digest should fail");
    if (!sign_fails(priv, a, NIL_HASH, crypto_hash_as_signer_opts(&sha256h)))
        testing_t_errorf_v(t, "Sign with empty digest should fail");

    /* opts.HashFunc() == 0 errors cleanly. */
    if (!sign_fails(priv, a, dg, crypto_hash_as_signer_opts(&zero)))
        testing_t_errorf_v(t, "Sign with crypto.Hash(0) should fail");

    arena_free(&ar);
}

static void TestSignHashLength(TestingT *t) {
    test_all_curves(t, sign_hash_length);
}

/* -------------------------------------------------- NonceSafety, INDCCA */

static void two_signatures(TestingT *t, EllipticCurve c, IoReader rand, const char *m0,
                           const char *m1, const char *same_s, const char *same_r) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;

    EcdsaPrivateKey *priv = ecdsa_generate_key(a, c, crypto_rand_reader, &err);
    BigInt *s0 = NULL, *s1 = NULL;
    BigInt *r0 = ecdsa_sign(a, rand, priv, cs(m0), &s0, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "error signing: %s", error_text(err));
        arena_free(&ar);
        return;
    }
    BigInt *r1 = ecdsa_sign(a, rand, priv, cs(m1), &s1, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "error signing: %s", error_text(err));
        arena_free(&ar);
        return;
    }

    if (big_int_cmp(s0, s1) == 0)
        testing_t_errorf_v(t, "%s", same_s);
    if (big_int_cmp(r0, r1) == 0)
        testing_t_errorf_v(t, "%s", same_r);

    arena_free(&ar);
}

static void nonce_safety(TestingT *t, EllipticCurve c, bool generic) {
    (void)generic;
    two_signatures(t, c, zero_reader(), "testing", "testing...",
                   "the signatures on two different messages were the same",
                   "the nonce used for two different messages was the same");
}

static void TestNonceSafety(TestingT *t) {
    test_all_curves(t, nonce_safety);
}

static void indcca(TestingT *t, EllipticCurve c, bool generic) {
    (void)generic;
    two_signatures(t, c, crypto_rand_reader, "testing", "testing",
                   "two signatures of the same message produced the same result",
                   "two signatures of the same message produced the same nonce");
}

static void TestINDCCA(TestingT *t) {
    test_all_curves(t, indcca);
}

/* -------------------------------------------------------------- Vectors */

/* testVectors: the NIST CAVP vectors of
 * https://csrc.nist.gov/groups/STM/cavp/documents/dss/186-3ecdsatestvectors.zip,
 * which Go has cut down to the algorithms it has and compressed. */
static void TestVectors(TestingT *t) {
    if (testing_short())
        return;

    Arena ar, rec;
    arena_init(&ar, NULL, 0);
    arena_init(&rec, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Alloc *ra = arena_allocator(&rec);

    BytesReader f;
    bytes_reader_reset(&f, bs(gen_sigver_rsp_bz2, sizeof gen_sigver_rsp_bz2));
    BufioReader *buf =
        bufio_new_reader(a, bzip2_new_reader(a, bytes_reader_as_io_reader(&f)));

    int line_no = 1;
    CryptoHash h = 0;
    bool have_curve = false;
    Slice msg = {0};
    BigInt *r = NULL, *s = NULL;
    EcdsaPublicKey pub = {{0}, NULL, NULL};

    for (;;) {
        Error err = BURROW_NO_ERROR;
        Str line = bufio_reader_read_string(buf, ra, '\n', &err);
        if (line.len == 0) {
            if (errors_is(err, io_eof))
                break;
            testing_t_fatalf_v(t, "error reading from input: %s", error_text(err));
        }
        line_no++;
        /* Need to remove \r\n from the end of the line. */
        if (!strings_has_suffix(line, BURROW_S("\r\n")))
            testing_t_fatalf_v(t, "bad line ending (expected \\r\\n) on line %d",
                               line_no);
        line.len -= 2;

        if (line.len == 0 || line.p[0] == '#')
            continue;

        if (line.p[0] == '[') {
            Str inner = {line.p + 1, line.len - 2};
            Str curve = inner, hash = {0};
            for (Int i = 0; i < inner.len; i++) {
                if (inner.p[i] == ',') {
                    curve.len = i;
                    hash = (Str){inner.p + i + 1, inner.len - i - 1};
                    break;
                }
            }

            have_curve = true;
            if (str_eq(curve, BURROW_S("P-224")))
                pub.curve = elliptic_p224();
            else if (str_eq(curve, BURROW_S("P-256")))
                pub.curve = elliptic_p256();
            else if (str_eq(curve, BURROW_S("P-384")))
                pub.curve = elliptic_p384();
            else if (str_eq(curve, BURROW_S("P-521")))
                pub.curve = elliptic_p521();
            else
                have_curve = false;

            if (str_eq(hash, BURROW_S("SHA-1")))
                h = CRYPTO_SHA1;
            else if (str_eq(hash, BURROW_S("SHA-224")))
                h = CRYPTO_SHA224;
            else if (str_eq(hash, BURROW_S("SHA-256")))
                h = CRYPTO_SHA256;
            else if (str_eq(hash, BURROW_S("SHA-384")))
                h = CRYPTO_SHA384;
            else if (str_eq(hash, BURROW_S("SHA-512")))
                h = CRYPTO_SHA512;
            else
                h = 0;

            arena_reset(&rec);
            continue;
        }

        if (h == 0 || !have_curve)
            continue;

        if (strings_has_prefix(line, BURROW_S("Msg = "))) {
            msg = hex_decode_string(ra, (Str){line.p + 6, line.len - 6}, &err);
            if (BURROW_FAILED(err))
                testing_t_fatalf_v(t, "failed to decode message on line %d: %s",
                                   line_no, error_text(err));
        } else if (strings_has_prefix(line, BURROW_S("Qx = ")) ||
                   strings_has_prefix(line, BURROW_S("Qy = ")) ||
                   strings_has_prefix(line, BURROW_S("R = ")) ||
                   strings_has_prefix(line, BURROW_S("S = "))) {
            Int skip = line.p[0] == 'Q' ? 5 : 4;
            BigInt *v = big_new_int(ra, 0);
            bool ok = false;
            big_int_set_string(v, (Str){line.p + skip, line.len - skip}, 16, &ok);
            if (!ok)
                panic_str(BURROW_S("bad hex"));
            if (line.p[0] == 'R')
                r = v;
            else if (line.p[0] == 'S')
                s = v;
            else if (line.p[1] == 'x')
                pub.x = v;
            else
                pub.y = v;
        } else if (strings_has_prefix(line, BURROW_S("Result = "))) {
            bool expected = line.p[9] == 'P';
            Slice hashed = digest(ra, h, msg);
            if (ecdsa_verify(&pub, hashed, r, s) != expected)
                testing_t_fatalf_v(t, "incorrect result on line %d", line_no);
            /* Every record sets all of Msg, Qx, Qy, R and S again. */
            arena_reset(&rec);
        } else {
            testing_t_fatalf_v(t, "unknown variable on line %d: %s", line_no, line);
        }
    }

    arena_free(&rec);
    arena_free(&ar);
}

/* ------------------------------------------------ signatures that fail */

static void negative_inputs(TestingT *t, EllipticCurve c, bool generic) {
    (void)generic;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;

    EcdsaPrivateKey *key = ecdsa_generate_key(a, c, crypto_rand_reader, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "failed to generate key");

    uint8_t hash[32] = {0};
    BigInt *r = big_new_int(a, 1);
    big_int_lsh(r, r, 550); /* larger than any supported curve */
    big_int_neg(r, r);

    if (ecdsa_verify(&key->public_key, bs(hash, sizeof hash), r, r))
        testing_t_errorf_v(t, "bogus signature accepted");

    arena_free(&ar);
}

static void TestNegativeInputs(TestingT *t) {
    test_all_curves(t, negative_inputs);
}

static const uint8_t zero_hash[64];

static void zero_hash_signature(TestingT *t, EllipticCurve c, bool generic) {
    (void)generic;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;

    EcdsaPrivateKey *priv = ecdsa_generate_key(a, c, crypto_rand_reader, &err);
    if (BURROW_FAILED(err))
        panic_str(error_text(err));

    /* Sign a hash consisting of all zeros. */
    BigInt *s = NULL;
    BigInt *r = ecdsa_sign(a, crypto_rand_reader, priv, bs(zero_hash, 64), &s, &err);
    if (BURROW_FAILED(err))
        panic_str(error_text(err));

    /* Confirm that it can be verified. */
    if (!ecdsa_verify(&priv->public_key, bs(zero_hash, 64), r, s))
        testing_t_errorf_v(t, "zero hash signature verify failed for %s",
                           elliptic_curve_params(c)->name);

    arena_free(&ar);
}

static void TestZeroHashSignature(TestingT *t) {
    test_all_curves(t, zero_hash_signature);
}

static void zero_signature(TestingT *t, EllipticCurve c, bool generic) {
    (void)generic;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;

    EcdsaPrivateKey *priv = ecdsa_generate_key(a, c, crypto_rand_reader, &err);
    if (BURROW_FAILED(err))
        panic_str(error_text(err));

    if (ecdsa_verify(&priv->public_key, bs(zero_hash, 64), big_new_int(a, 0),
                     big_new_int(a, 0)))
        testing_t_errorf_v(t, "Verify with r,s=0 succeeded: %s",
                           elliptic_curve_params(c)->name);

    arena_free(&ar);
}

static void TestZeroSignature(TestingT *t) {
    test_all_curves(t, zero_signature);
}

/* What testNegativeSignature, testRPlusNSignature and testRMinusNSignature do
 * to r before checking the signature fails. */
typedef enum { NEGATE_R, R_PLUS_N, R_MINUS_N } RChange;

static void changed_r_signature(TestingT *t, EllipticCurve c, RChange change,
                                const char *what) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;

    EcdsaPrivateKey *priv = ecdsa_generate_key(a, c, crypto_rand_reader, &err);
    if (BURROW_FAILED(err))
        panic_str(error_text(err));
    BigInt *s = NULL;
    BigInt *r = ecdsa_sign(a, crypto_rand_reader, priv, bs(zero_hash, 64), &s, &err);
    if (BURROW_FAILED(err))
        panic_str(error_text(err));

    const BigInt *n = elliptic_curve_params(c)->n;
    if (change == NEGATE_R)
        big_int_neg(r, r);
    else if (change == R_PLUS_N)
        big_int_add(r, r, n);
    else
        big_int_sub(r, r, n);
    if (ecdsa_verify(&priv->public_key, bs(zero_hash, 64), r, s))
        testing_t_errorf_v(t, "Verify with %s succeeded: %s", what,
                           elliptic_curve_params(c)->name);

    arena_free(&ar);
}

static void negative_signature(TestingT *t, EllipticCurve c, bool generic) {
    (void)generic;
    changed_r_signature(t, c, NEGATE_R, "r=-r");
}

static void TestNegativeSignature(TestingT *t) {
    test_all_curves(t, negative_signature);
}

static void r_plus_n_signature(TestingT *t, EllipticCurve c, bool generic) {
    (void)generic;
    changed_r_signature(t, c, R_PLUS_N, "r=r+n");
}

static void TestRPlusNSignature(TestingT *t) {
    test_all_curves(t, r_plus_n_signature);
}

static void r_minus_n_signature(TestingT *t, EllipticCurve c, bool generic) {
    (void)generic;
    changed_r_signature(t, c, R_MINUS_N, "r=r-n");
}

static void TestRMinusNSignature(TestingT *t) {
    test_all_curves(t, r_minus_n_signature);
}

/* -------------------------------------------------------------- RFC6979 */

typedef struct Rfc6979Vector {
    const char *curve, *d, *x, *y, *msg, *r, *s;
} Rfc6979Vector;

/* The examples of RFC 6979, appendix A.2, with SHA-256. */
static const Rfc6979Vector rfc6979_vectors[] = {
    {"P224", "F220266E1105BFE3083E03EC7A3A654651F45E37167E88600BF257C1",
     "00CF08DA5AD719E42707FA431292DEA11244D64FC51610D94B130D6C",
     "EEAB6F3DEBE455E3DBF85416F7030CBD94F34F2D6F232C69F3C1385A", "sample",
     "61AA3DA010E8E8406C656BC477A7A7189895E7E840CDFE8FF42307BA",
     "BC814050DAB5D23770879494F9E0A680DC1AF7161991BDE692B10101"},
    {"P224", "F220266E1105BFE3083E03EC7A3A654651F45E37167E88600BF257C1",
     "00CF08DA5AD719E42707FA431292DEA11244D64FC51610D94B130D6C",
     "EEAB6F3DEBE455E3DBF85416F7030CBD94F34F2D6F232C69F3C1385A", "test",
     "AD04DDE87B84747A243A631EA47A1BA6D1FAA059149AD2440DE6FBA6",
     "178D49B1AE90E3D8B629BE3DB5683915F4E8C99FDF6E666CF37ADCFD"},
    {"P256", "C9AFA9D845BA75166B5C215767B1D6934E50C3DB36E89B127B8A622B120F6721",
     "60FED4BA255A9D31C961EB74C6356D68C049B8923B61FA6CE669622E60F29FB6",
     "7903FE1008B8BC99A41AE9E95628BC64F2F1B20C2D7E9F5177A3C294D4462299", "wv[vnX",
     "EFD9073B652E76DA1B5A019C0E4A2E3FA529B035A6ABB91EF67F0ED7A1F21234",
     "3DB4706C9D9F4A4FE13BB5E08EF0FAB53A57DBAB2061C83A35FA411C68D2BA33"},
    {"P256", "C9AFA9D845BA75166B5C215767B1D6934E50C3DB36E89B127B8A622B120F6721",
     "60FED4BA255A9D31C961EB74C6356D68C049B8923B61FA6CE669622E60F29FB6",
     "7903FE1008B8BC99A41AE9E95628BC64F2F1B20C2D7E9F5177A3C294D4462299", "sample",
     "EFD48B2AACB6A8FD1140DD9CD45E81D69D2C877B56AAF991C34D0EA84EAF3716",
     "F7CB1C942D657C41D436C7A1B6E29F65F3E900DBB9AFF4064DC4AB2F843ACDA8"},
    {"P256", "C9AFA9D845BA75166B5C215767B1D6934E50C3DB36E89B127B8A622B120F6721",
     "60FED4BA255A9D31C961EB74C6356D68C049B8923B61FA6CE669622E60F29FB6",
     "7903FE1008B8BC99A41AE9E95628BC64F2F1B20C2D7E9F5177A3C294D4462299", "test",
     "F1ABB023518351CD71D881567B1EA663ED3EFCF6C5132B354F28D3B0B7D38367",
     "019F4113742A2B14BD25926B49C649155F267E60D3814B4C0CC84250E46F0083"},
    {"P384",
     "6B9D3DAD2E1B8C1C05B19875B6659F4DE23C3B667BF297BA9AA47740787137D896D5724E4C70A825F"
     "872C9EA60D2EDF5",
     "EC3A4E415B4E19A4568618029F427FA5DA9A8BC4AE92E02E06AAE5286B300C64DEF8F0EA905586606"
     "4A254515480BC13",
     "8015D9B72D7D57244EA8EF9AC0C621896708A59367F9DFB9F54CA84B3F1C9DB1288B231C3AE0D4FE7"
     "344FD2533264720",
     "sample",
     "21B13D1E013C7FA1392D03C5F99AF8B30C570C6F98D4EA8E354B63A21D3DAA33BDE1E888E63355D92"
     "FA2B3C36D8FB2CD",
     "F3AA443FB107745BF4BD77CB3891674632068A10CA67E3D45DB2266FA7D1FEEBEFDC63ECCD1AC42EC"
     "0CB8668A4FA0AB0"},
    {"P384",
     "6B9D3DAD2E1B8C1C05B19875B6659F4DE23C3B667BF297BA9AA47740787137D896D5724E4C70A825F"
     "872C9EA60D2EDF5",
     "EC3A4E415B4E19A4568618029F427FA5DA9A8BC4AE92E02E06AAE5286B300C64DEF8F0EA905586606"
     "4A254515480BC13",
     "8015D9B72D7D57244EA8EF9AC0C621896708A59367F9DFB9F54CA84B3F1C9DB1288B231C3AE0D4FE7"
     "344FD2533264720",
     "test",
     "6D6DEFAC9AB64DABAFE36C6BF510352A4CC27001263638E5B16D9BB51D451559F918EEDAF2293BE5B"
     "475CC8F0188636B",
     "2D46F3BECBCC523D5F1A1256BF0C9B024D879BA9E838144C8BA6BAEB4B53B47D51AB373F9845C0514"
     "EEFB14024787265"},
    {"P521",
     "0FAD06DAA62BA3B25D2FB40133DA757205DE67F5BB0018FEE8C86E1B68C7E75CAA896EB32F1F47C70"
     "855836A6D16FCC1466F6D8FBEC67DB89EC0C08B0E996B83538",
     "1894550D0785932E00EAA23B694F213F8C3121F86DC97A04E5A7167DB4E5BCD371123D46E45DB6B5D"
     "5370A7F20FB633155D38FFA16D2BD761DCAC474B9A2F5023A4",
     "0493101C962CD4D2FDDF782285E64584139C2F91B47F87FF82354D6630F746A28A0DB25741B5B34A8"
     "28008B22ACC23F924FAAFBD4D33F81EA66956DFEAA2BFDFCF5",
     "sample",
     "1511BB4D675114FE266FC4372B87682BAECC01D3CC62CF2303C92B3526012659D16876E25C7C1E576"
     "48F23B73564D67F61C6F14D527D54972810421E7D87589E1A7",
     "04A171143A83163D6DF460AAF61522695F207A58B95C0644D87E52AA1A347916E4F7A72930B1BC06D"
     "BE22CE3F58264AFD23704CBB63B29B931F7DE6C9D949A7ECFC"},
    {"P521",
     "0FAD06DAA62BA3B25D2FB40133DA757205DE67F5BB0018FEE8C86E1B68C7E75CAA896EB32F1F47C70"
     "855836A6D16FCC1466F6D8FBEC67DB89EC0C08B0E996B83538",
     "1894550D0785932E00EAA23B694F213F8C3121F86DC97A04E5A7167DB4E5BCD371123D46E45DB6B5D"
     "5370A7F20FB633155D38FFA16D2BD761DCAC474B9A2F5023A4",
     "0493101C962CD4D2FDDF782285E64584139C2F91B47F87FF82354D6630F746A28A0DB25741B5B34A8"
     "28008B22ACC23F924FAAFBD4D33F81EA66956DFEAA2BFDFCF5",
     "test",
     "00E871C4A14F993C6C7369501900C4BC1E9C7B0B4BA44E04868B30B41D8071042EB28C4C250411D0C"
     "E08CD197E4188EA4876F279F90B3D8D74A3C76E6F1E4656AA8",
     "0CD52DBAA33B063C3A6CD8058A1FB0A46A4754B034FCC644766CA14DA8CA5CA9FDE00E88C1AD60CCB"
     "A759025299079D7A427EC3CC5B619BFBC828E7769BCD694E86"},
};

typedef struct SigEnv {
    Slice r, s;
} SigEnv;

static void asn1_int_body(void *env, CryptobyteBuilder *c) {
    Slice bytes = *(const Slice *)env;
    if (((const uint8_t *)bytes.p)[0] & 0x80)
        cryptobyte_builder_add_uint8(c, 0);
    cryptobyte_builder_add_bytes(c, bytes);
}

/* addASN1IntBytes. */
static void add_asn1_int_bytes(CryptobyteBuilder *b, Slice bytes) {
    while (bytes.len > 0 && ((const uint8_t *)bytes.p)[0] == 0) {
        bytes.p = (uint8_t *)bytes.p + 1;
        bytes.len--;
        bytes.cap--;
    }
    cryptobyte_builder_add_asn1(
        b, CRYPTOBYTE_ASN1_INTEGER,
        BURROW_FN(CryptobyteBuilderContinuation, asn1_int_body, &bytes));
}

static void signature_body(void *env, CryptobyteBuilder *b) {
    const SigEnv *e = env;
    add_asn1_int_bytes(b, e->r);
    add_asn1_int_bytes(b, e->s);
}

/* encodeSignature, from a. */
static Slice encode_signature(Alloc *a, Slice r, Slice s, Error *err) {
    CryptobyteBuilder b = cryptobyte_new_builder(a, (Slice){0});
    SigEnv e = {r, s};
    cryptobyte_builder_add_asn1(
        &b, CRYPTOBYTE_ASN1_SEQUENCE,
        BURROW_FN(CryptobyteBuilderContinuation, signature_body, &e));
    return cryptobyte_builder_bytes(&b, err);
}

static void rfc6979_one(TestingT *t, Alloc *a, const Rfc6979Vector *v) {
    EcdsaPrivateKey priv = {
        {curve_by_name(v->curve), from_hex(a, v->x), from_hex(a, v->y)},
        from_hex(a, v->d),
    };
    Sha256Sum256Ret h = sha256_sum256(cs(v->msg));
    CryptoHash sha256h = CRYPTO_SHA256;
    Error err = BURROW_NO_ERROR;
    Slice sig = ecdsa_private_key_sign(&priv, a, (IoReader){0}, bs(h.a, sizeof h.a),
                                       crypto_hash_as_signer_opts(&sha256h), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    Slice expected = encode_signature(a, big_int_bytes(from_hex(a, v->r), a),
                                      big_int_bytes(from_hex(a, v->s), a), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    if (!bytes_equal(sig, expected))
        testing_t_errorf_v(t, "signature mismatch:\n got: %s\nwant: %s", hexs(a, sig),
                           hexs(a, expected));
}

static void rfc6979_curve(void *env, TestingT *t) {
    const char *curve = env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof rfc6979_vectors / sizeof rfc6979_vectors[0]; i++)
        if (strcmp(rfc6979_vectors[i].curve, curve) == 0)
            rfc6979_one(t, a, &rfc6979_vectors[i]);
    arena_free(&ar);
}

static void TestRFC6979(TestingT *t) {
    static const struct {
        const char *name, *curve;
    } curves[] = {
        {"P-224", "P224"}, {"P-256", "P256"}, {"P-384", "P384"}, {"P-521", "P521"}};
    for (size_t i = 0; i < sizeof curves / sizeof curves[0]; i++)
        testing_t_run(
            t, str_from_cstr(curves[i].name),
            BURROW_FN(TestingTFunc, rfc6979_curve, (void *)(uintptr_t)curves[i].curve));
}

/* ------------------------------------------------ ParseAndBytesRoundTrip */

static void parse_and_bytes_round_trip(TestingT *t, EllipticCurve c, bool generic) {
    if (generic)
        testing_t_skip_v(t, "these methods don't support generic curves");
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;

    EcdsaPrivateKey *priv = ecdsa_generate_key(a, c, crypto_rand_reader, &err);

    Slice b = ecdsa_public_key_bytes(&priv->public_key, a, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "failed to serialize private key's public key: %s",
                           error_text(err));
    if (((const uint8_t *)b.p)[0] != 4)
        testing_t_fatalf_v(t, "public key bytes doesn't start with 0x04 (uncompressed "
                              "format)");
    EcdsaPublicKey *p = ecdsa_parse_uncompressed_public_key(a, c, b, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "failed to parse private key's public key: %s",
                           error_text(err));
    if (!ecdsa_public_key_equal(&priv->public_key,
                                BURROW_ANY(TYPE_ECDSA_PUBLIC_KEY, p)))
        testing_t_errorf_v(t, "parsed private key's public key doesn't match original");

    Slice bk = ecdsa_private_key_bytes(priv, a, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "failed to serialize private key: %s", error_text(err));
    EcdsaPrivateKey *k = ecdsa_parse_raw_private_key(a, c, bk, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "failed to parse private key: %s", error_text(err));
    if (!ecdsa_private_key_equal(priv, BURROW_ANY(TYPE_ECDSA_PRIVATE_KEY, k)))
        testing_t_errorf_v(t, "parsed private key doesn't match original");

    if (!same_curve(c, elliptic_p224())) {
        EcdhPrivateKey *priv_ecdh = ecdsa_private_key_ecdh(priv, a, &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "failed to convert private key to ECDH: %s",
                               error_text(err));

        const EcdhCurve *curve = ecdh_private_key_curve(priv_ecdh);
        EcdhPublicKey *pp = ecdh_curve_new_public_key(curve, a, b, &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "failed to parse with ECDH: %s", error_text(err));
        if (!ecdh_public_key_equal(ecdh_private_key_public_key(priv_ecdh),
                                   BURROW_ANY(TYPE_ECDH_PUBLIC_KEY, pp)))
            testing_t_errorf_v(t, "parsed ECDH public key doesn't match original");
        if (!bytes_equal(b, ecdh_public_key_bytes(pp, a)))
            testing_t_errorf_v(t, "encoded ECDH public key doesn't match Bytes");

        EcdhPrivateKey *kk = ecdh_curve_new_private_key(curve, a, bk, &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "failed to parse with ECDH: %s", error_text(err));
        if (!ecdh_private_key_equal(priv_ecdh, BURROW_ANY(TYPE_ECDH_PRIVATE_KEY, kk)))
            testing_t_errorf_v(t, "parsed ECDH private key doesn't match original");
        if (!bytes_equal(bk, ecdh_private_key_bytes(kk, a)))
            testing_t_errorf_v(t, "encoded ECDH private key doesn't match Bytes");
    }

    arena_free(&ar);
}

static void TestParseAndBytesRoundTrip(TestingT *t) {
    test_all_curves(t, parse_and_bytes_round_trip);
}

/* -------------------------------------------------------- InvalidKeys */

static Int field_bytes(EllipticCurve c) {
    return (elliptic_curve_params(c)->bit_size + 7) / 8;
}

static void invalid_public_infinity(void *env, TestingT *t) {
    EllipticCurve c = *(const EllipticCurve *)env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    EcdsaPublicKey k = {c, big_new_int(a, 0), big_new_int(a, 0)};
    Error err = BURROW_NO_ERROR;
    (void)ecdsa_public_key_bytes(&k, a, &err);
    if (!BURROW_FAILED(err))
        testing_t_errorf_v(t, "PublicKey.Bytes accepted infinity");

    uint8_t b[NISTEC_MAX_POINT_BYTES] = {0};
    err = BURROW_NO_ERROR;
    (void)ecdsa_parse_uncompressed_public_key(a, c, bs(b, 1), &err);
    if (!BURROW_FAILED(err))
        testing_t_errorf_v(t, "ParseUncompressedPublicKey accepted infinity");
    b[0] = 4;
    err = BURROW_NO_ERROR;
    (void)ecdsa_parse_uncompressed_public_key(a, c, bs(b, 1 + 2 * field_bytes(c)),
                                              &err);
    if (!BURROW_FAILED(err))
        testing_t_errorf_v(t, "ParseUncompressedPublicKey accepted infinity");

    arena_free(&ar);
}

static void invalid_public_not_on_curve(void *env, TestingT *t) {
    EllipticCurve c = *(const EllipticCurve *)env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    Error err = BURROW_NO_ERROR;
    EcdsaPrivateKey *k = ecdsa_generate_key(a, c, crypto_rand_reader, &err);
    big_int_add(k->public_key.x, k->public_key.x, big_new_int(a, 1));
    (void)ecdsa_public_key_bytes(&k->public_key, a, &err);
    if (!BURROW_FAILED(err))
        testing_t_errorf_v(t, "PublicKey.Bytes accepted not on curve");

    Int n = field_bytes(c);
    uint8_t b[NISTEC_MAX_POINT_BYTES] = {4};
    big_int_fill_bytes(k->public_key.x, bs(b + 1, n));
    big_int_fill_bytes(k->public_key.y, bs(b + 1 + n, n));
    err = BURROW_NO_ERROR;
    (void)ecdsa_parse_uncompressed_public_key(a, c, bs(b, 1 + 2 * n), &err);
    if (!BURROW_FAILED(err))
        testing_t_errorf_v(t, "ParseUncompressedPublicKey accepted not on curve");

    arena_free(&ar);
}

static void invalid_public_compressed(void *env, TestingT *t) {
    EllipticCurve c = *(const EllipticCurve *)env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    Error err = BURROW_NO_ERROR;
    EcdsaPrivateKey *k = ecdsa_generate_key(a, c, crypto_rand_reader, &err);
    Slice b = elliptic_marshal_compressed(a, c, k->public_key.x, k->public_key.y);
    (void)ecdsa_parse_uncompressed_public_key(a, c, b, &err);
    if (!BURROW_FAILED(err))
        testing_t_errorf_v(t, "ParseUncompressedPublicKey accepted compressed key");

    arena_free(&ar);
}

static void invalid_public_keys(TestingT *t, EllipticCurve c, bool generic) {
    (void)generic;
    testing_t_run(t, BURROW_S("Infinity"),
                  BURROW_FN(TestingTFunc, invalid_public_infinity, &c));
    testing_t_run(t, BURROW_S("NotOnCurve"),
                  BURROW_FN(TestingTFunc, invalid_public_not_on_curve, &c));
    testing_t_run(t, BURROW_S("Compressed"),
                  BURROW_FN(TestingTFunc, invalid_public_compressed, &c));
}

static void TestInvalidPublicKeys(TestingT *t) {
    test_all_curves(t, invalid_public_keys);
}

static void invalid_private_zero(void *env, TestingT *t) {
    EllipticCurve c = *(const EllipticCurve *)env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    EcdsaPrivateKey k = {{c, big_new_int(a, 0), big_new_int(a, 0)}, big_new_int(a, 0)};
    Error err = BURROW_NO_ERROR;
    (void)ecdsa_private_key_bytes(&k, a, &err);
    if (!BURROW_FAILED(err))
        testing_t_errorf_v(t, "PrivateKey.Bytes accepted zero key");

    uint8_t b[NISTEC_MAX_ELEMENT_BYTES] = {0};
    err = BURROW_NO_ERROR;
    (void)ecdsa_parse_raw_private_key(a, c, bs(b, field_bytes(c)), &err);
    if (!BURROW_FAILED(err))
        testing_t_errorf_v(t, "ParseRawPrivateKey accepted zero key");

    arena_free(&ar);
}

static void invalid_private_overflow(void *env, TestingT *t) {
    EllipticCurve c = *(const EllipticCurve *)env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    BigInt *d = big_new_int(a, 5);
    big_int_add(d, elliptic_curve_params(c)->n, d);
    BigInt *y = NULL;
    BigInt *x = elliptic_curve_scalar_base_mult(c, a, big_int_bytes(d, a), &y);
    EcdsaPrivateKey k = {{c, x, y}, d};
    Error err = BURROW_NO_ERROR;
    (void)ecdsa_private_key_bytes(&k, a, &err);
    if (!BURROW_FAILED(err))
        testing_t_errorf_v(t, "PrivateKey.Bytes accepted overflow key");

    uint8_t b[NISTEC_MAX_ELEMENT_BYTES] = {0};
    Int n = field_bytes(c);
    big_int_fill_bytes(d, bs(b, n));
    err = BURROW_NO_ERROR;
    (void)ecdsa_parse_raw_private_key(a, c, bs(b, n), &err);
    if (!BURROW_FAILED(err))
        testing_t_errorf_v(t, "ParseRawPrivateKey accepted overflow key");

    arena_free(&ar);
}

static void invalid_private_length(void *env, TestingT *t) {
    EllipticCurve c = *(const EllipticCurve *)env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    static const uint8_t short_key[] = {1, 2, 3};
    Error err = BURROW_NO_ERROR;
    (void)ecdsa_parse_raw_private_key(a, c, bs(short_key, 3), &err);
    if (!BURROW_FAILED(err))
        testing_t_errorf_v(t, "ParseRawPrivateKey accepted short key");

    uint8_t b[NISTEC_MAX_ELEMENT_BYTES + 3] = {0};
    Int n = field_bytes(c);
    memcpy(b + n, short_key, 3);
    err = BURROW_NO_ERROR;
    (void)ecdsa_parse_raw_private_key(a, c, bs(b, n + 3), &err);
    if (!BURROW_FAILED(err))
        testing_t_errorf_v(t, "ParseRawPrivateKey accepted long key");

    arena_free(&ar);
}

static void invalid_private_keys(TestingT *t, EllipticCurve c, bool generic) {
    (void)generic;
    testing_t_run(t, BURROW_S("Zero"),
                  BURROW_FN(TestingTFunc, invalid_private_zero, &c));
    testing_t_run(t, BURROW_S("Overflow"),
                  BURROW_FN(TestingTFunc, invalid_private_overflow, &c));
    testing_t_run(t, BURROW_S("Length"),
                  BURROW_FN(TestingTFunc, invalid_private_length, &c));
}

static void TestInvalidPrivateKeys(TestingT *t) {
    test_all_curves(t, invalid_private_keys);
}

/* ---------------------------------------------------------------- PKCS8 */

/* The curves' object identifiers, as the content of an OBJECT IDENTIFIER. */
static const struct {
    const char *name;
    uint8_t oid[8];
    Int len;
} curve_oids[] = {
    {"P-224", {0x2b, 0x81, 0x04, 0x00, 0x21}, 5},
    {"P-256", {0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07}, 8},
    {"P-384", {0x2b, 0x81, 0x04, 0x00, 0x22}, 5},
    {"P-521", {0x2b, 0x81, 0x04, 0x00, 0x23}, 5},
};

/* id-ecPublicKey, 1.2.840.10045.2.1. */
static const uint8_t oid_public_key_ecdsa[] = {0x2a, 0x86, 0x48, 0xce,
                                               0x3d, 0x02, 0x01};

/* DER, written into a buffer big enough for any of these keys. */
typedef struct Der {
    uint8_t b[512];
    Int n;
} Der;

static void der_put(Der *d, const void *p, Int n) {
    if (d->n + n > (Int)sizeof d->b)
        panic_str(BURROW_S("der: buffer too small"));
    memcpy(d->b + d->n, p, (size_t)n);
    d->n += n;
}

/* An element with tag and content, appended to d. */
static void der_element(Der *d, uint8_t tag, const void *content, Int n) {
    uint8_t head[4] = {tag};
    Int hn = 1;
    if (n < 0x80) {
        head[hn++] = (uint8_t)n;
    } else if (n < 0x100) {
        head[hn++] = 0x81;
        head[hn++] = (uint8_t)n;
    } else {
        head[hn++] = 0x82;
        head[hn++] = (uint8_t)(n >> 8);
        head[hn++] = (uint8_t)n;
    }
    der_put(d, head, hn);
    der_put(d, content, n);
}

/* x509.MarshalPKCS8PrivateKey for an ECDSA key, from a: a PrivateKeyInfo with
 * id-ecPublicKey and the curve's identifier, around the ECPrivateKey of RFC
 * 5915 without the optional curve parameters, which marshalECPrivateKeyWithOID
 * leaves out when PKCS #8 already has them. */
static Slice pkcs8_marshal(Alloc *a, const EcdsaPrivateKey *k, Error *err) {
    const char *name = NULL;
    Str cname = elliptic_curve_params(k->public_key.curve)->name;
    for (size_t i = 0; i < sizeof curve_oids / sizeof curve_oids[0]; i++)
        if (str_eq(cname, str_from_cstr(curve_oids[i].name)) &&
            same_curve(k->public_key.curve, curve_by_name(curve_oids[i].name)))
            name = curve_oids[i].name;
    if (name == NULL) {
        *err = errors_new(error_allocator(),
                          BURROW_S("x509: unknown curve while marshaling to PKCS#8"));
        return (Slice){0};
    }
    Slice priv = ecdsa_private_key_bytes(k, a, err);
    if (BURROW_FAILED(*err))
        return (Slice){0};
    Slice pub = ecdsa_public_key_bytes(&k->public_key, a, err);
    if (BURROW_FAILED(*err))
        return (Slice){0};

    /* ECPrivateKey: version 1, the scalar, and [1] the public key. */
    Der bits = {{0}, 0}, explicit = {{0}, 0}, ec = {{0}, 0};
    der_put(&bits, "\0", 1);
    der_put(&bits, pub.p, pub.len);
    der_element(&explicit, 0x03, bits.b, bits.n);
    static const uint8_t one[] = {0x02, 0x01, 0x01};
    Der ec_body = {{0}, 0};
    der_put(&ec_body, one, sizeof one);
    der_element(&ec_body, 0x04, priv.p, priv.len);
    der_element(&ec_body, 0xa1, explicit.b, explicit.n);
    der_element(&ec, 0x30, ec_body.b, ec_body.n);

    /* PrivateKeyInfo: version 0, the AlgorithmIdentifier and the key. */
    Der algo = {{0}, 0}, body = {{0}, 0}, out = {{0}, 0};
    der_element(&algo, 0x06, oid_public_key_ecdsa, sizeof oid_public_key_ecdsa);
    for (size_t i = 0; i < sizeof curve_oids / sizeof curve_oids[0]; i++)
        if (strcmp(curve_oids[i].name, name) == 0)
            der_element(&algo, 0x06, curve_oids[i].oid, curve_oids[i].len);
    static const uint8_t zero[] = {0x02, 0x01, 0x00};
    der_put(&body, zero, sizeof zero);
    der_element(&body, 0x30, algo.b, algo.n);
    der_element(&body, 0x04, ec.b, ec.n);
    der_element(&out, 0x30, body.b, body.n);

    return bytes_clone(a, bs(out.b, out.n));
}

/* x509.ParsePKCS8PrivateKey for an ECDSA key, from a, which comes down to
 * ecdsa_parse_raw_private_key on the scalar inside. */
static EcdsaPrivateKey *pkcs8_parse(Alloc *a, Slice der, Error *err) {
    CryptobyteString input = der, info, algo, oid, ec_der, ec, d;
    if (!cryptobyte_string_read_asn1(&input, &info, CRYPTOBYTE_ASN1_SEQUENCE) ||
        !cryptobyte_string_empty(input) ||
        !cryptobyte_string_skip_asn1(&info, CRYPTOBYTE_ASN1_INTEGER) ||
        !cryptobyte_string_read_asn1(&info, &algo, CRYPTOBYTE_ASN1_SEQUENCE) ||
        !cryptobyte_string_skip_asn1(&algo, CRYPTOBYTE_ASN1_OBJECT_IDENTIFIER) ||
        !cryptobyte_string_read_asn1(&algo, &oid, CRYPTOBYTE_ASN1_OBJECT_IDENTIFIER) ||
        !cryptobyte_string_read_asn1(&info, &ec_der, CRYPTOBYTE_ASN1_OCTET_STRING) ||
        !cryptobyte_string_read_asn1(&ec_der, &ec, CRYPTOBYTE_ASN1_SEQUENCE) ||
        !cryptobyte_string_skip_asn1(&ec, CRYPTOBYTE_ASN1_INTEGER) ||
        !cryptobyte_string_read_asn1(&ec, &d, CRYPTOBYTE_ASN1_OCTET_STRING)) {
        *err = errors_new(error_allocator(), BURROW_S("x509: malformed PKCS#8"));
        return NULL;
    }
    for (size_t i = 0; i < sizeof curve_oids / sizeof curve_oids[0]; i++)
        if (bytes_equal(oid, bs(curve_oids[i].oid, curve_oids[i].len)))
            return ecdsa_parse_raw_private_key(a, curve_by_name(curve_oids[i].name), d,
                                               err);
    *err = errors_new(error_allocator(), BURROW_S("x509: unknown elliptic curve"));
    return NULL;
}

/* ------------------------------------------------ KeyGenerationVectors */

/* keyGenTestReader: a read of one byte, which is randutil.MaybeReadByte, gets
 * nothing out of the DRBG, and every other read is filled from it. */
static Int key_gen_read(void *self, Slice p, Error *err) {
    (void)err;
    if (p.len == 1)
        return 1;
    burrow__ecdsa_drbg_generate(self, p);
    return p.len;
}

static const IoReaderVT key_gen_vt = {&test_type, key_gen_read};

static void key_generation_vector(void *env, TestingT *t) {
    const GenKeyGen *v = env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    EllipticCurve curve = curve_by_name(v->curve);
    if (curve.vt == NULL)
        testing_t_fatalf_v(t, "unknown curve: %q", v->curve);
    Str name = elliptic_curve_params(curve)->name;
    char pers[32];
    snprintf(pers, sizeof pers, "det ECDSA key gen %.*s", (int)name.len,
             (const char *)name.p);
    burrow__crypto_rand_godebug_set("cryptocustomrand=1");
    EcdsaDrbg *drbg =
        burrow__ecdsa_new_drbg(a, sha256_new, unhex(a, v->seed), (Slice){0}, cs(pers));
    IoReader rng = {&key_gen_vt, drbg};
    Error err = BURROW_NO_ERROR;
    EcdsaPrivateKey *priv = ecdsa_generate_key(a, curve, rng, &err);
    burrow__crypto_rand_godebug_set(NULL);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "GenerateKey: %s", error_text(err));
    Slice der = pkcs8_marshal(a, priv, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "MarshalPKCS8PrivateKey: %s", error_text(err));
    Slice want = unhex(a, v->pkcs8);
    if (!bytes_equal(der, want))
        testing_t_errorf_v(t, "PKCS8 mismatch:\n%s\nvs\n\n%s", hexs(a, der),
                           hexs(a, want));

    arena_free(&ar);
}

/* TestKeyGenerationVectors: GenerateKey with the deterministic key generation
 * vectors of c2sp.org/det-keygen, with the DRBG they give in place of the
 * random source. */
static void TestKeyGenerationVectors(TestingT *t) {
    for (size_t i = 0; i < sizeof gen_keygen / sizeof gen_keygen[0]; i++) {
        char name[48];
        snprintf(name, sizeof name, "%s-%d", gen_keygen[i].curve, (int)i);
        testing_t_run(t, str_from_cstr(name),
                      BURROW_FN(TestingTFunc, key_generation_vector,
                                (void *)(uintptr_t)&gen_keygen[i]));
    }
}

/* ---------------------------------------------------------------- Equal */

static void equal_one(void *env, TestingT *t) {
    EllipticCurve c = *(const EllipticCurve *)env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;

    EcdsaPrivateKey *priv = ecdsa_generate_key(a, c, crypto_rand_reader, &err);
    EcdsaPublicKey *public = &priv->public_key;

    if (!ecdsa_public_key_equal(public, BURROW_ANY(TYPE_ECDSA_PUBLIC_KEY, public)))
        testing_t_errorf_v(t, "public key is not equal to itself");
    if (!ecdsa_public_key_equal(public,
                                crypto_signer_public(ecdsa_private_key_signer(priv))))
        testing_t_errorf_v(t, "private.Public() is not Equal to public");
    if (!ecdsa_private_key_equal(priv, BURROW_ANY(TYPE_ECDSA_PRIVATE_KEY, priv)))
        testing_t_errorf_v(t, "private key is not equal to itself");

    Slice enc = pkcs8_marshal(a, priv, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    EcdsaPrivateKey *decoded = pkcs8_parse(a, enc, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    if (!ecdsa_public_key_equal(public, ecdsa_private_key_public(decoded)))
        testing_t_errorf_v(t, "public key is not equal to itself after decoding");
    if (!ecdsa_private_key_equal(priv, BURROW_ANY(TYPE_ECDSA_PRIVATE_KEY, decoded)))
        testing_t_errorf_v(t, "private key is not equal to itself after decoding");

    EcdsaPrivateKey *other = ecdsa_generate_key(a, c, crypto_rand_reader, &err);
    if (ecdsa_public_key_equal(public, ecdsa_private_key_public(other)))
        testing_t_errorf_v(t, "different public keys are Equal");
    if (ecdsa_private_key_equal(priv, BURROW_ANY(TYPE_ECDSA_PRIVATE_KEY, other)))
        testing_t_errorf_v(t, "different private keys are Equal");

    /* Ensure that keys with the same coordinates but on different curves
     * aren't considered Equal. */
    EcdsaPublicKey different_curve = *public;
    different_curve.curve = same_curve(different_curve.curve, elliptic_p256())
                                ? elliptic_p224()
                                : elliptic_p256();
    if (ecdsa_public_key_equal(public,
                               BURROW_ANY(TYPE_ECDSA_PUBLIC_KEY, &different_curve)))
        testing_t_errorf_v(t, "public keys with different curves are Equal");

    arena_free(&ar);
}

static void TestEqual(TestingT *t) {
    EllipticCurve p224 = elliptic_p224(), p256 = elliptic_p256(),
                  p384 = elliptic_p384(), p521 = elliptic_p521();
    testing_t_run(t, BURROW_S("P224"), BURROW_FN(TestingTFunc, equal_one, &p224));
    if (testing_short())
        return;
    testing_t_run(t, BURROW_S("P256"), BURROW_FN(TestingTFunc, equal_one, &p256));
    testing_t_run(t, BURROW_S("P384"), BURROW_FN(TestingTFunc, equal_one, &p384));
    testing_t_run(t, BURROW_S("P521"), BURROW_FN(TestingTFunc, equal_one, &p521));
}

/* ----------------------------------------------------------- Wycheproof */

/* The signature of a Wycheproof test, which the generator has cut into
 * pieces C99 can take as literals. */
static Slice unhex_parts(Alloc *a, const char *const parts[3]) {
    Slice b[3];
    Int n = 0;
    while (n < 3 && parts[n] != NULL) {
        b[n] = unhex(a, parts[n]);
        n++;
    }
    return slices_concat(a, b, n);
}

static void TestECDSAWycheproof(TestingT *t) {
    Arena ar, keys;
    arena_init(&ar, NULL, 0);
    arena_init(&keys, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    int group = -1;
    EcdsaPublicKey *pubkey = NULL;
    for (size_t i = 0; i < sizeof gen_wycheproof / sizeof gen_wycheproof[0]; i++) {
        const GenWycheproof *tv = &gen_wycheproof[i];
        const GenWycheproofGroup *tg = &gen_wycheproof_groups[tv->group];
        if (tv->group != group) {
            arena_reset(&keys);
            Error err = BURROW_NO_ERROR;
            pubkey = ecdsa_parse_uncompressed_public_key(arena_allocator(&keys),
                                                         curve_by_name(tg->curve),
                                                         unhex(a, tg->pub), &err);
            if (BURROW_FAILED(err))
                testing_t_fatalf_v(t, "%s: test group %d invalid public key: %s",
                                   tg->file, tv->group + 1, error_text(err));
            group = tv->group;
        }

        Slice hashed = digest(a, tg->hash, unhex(a, tv->msg));
        bool got = ecdsa_verify_asn1(pubkey, hashed, unhex_parts(a, tv->sig));
        if (got != tv->want)
            testing_t_errorf_v(t, "%s #%d %s: VerifyASN1 wanted success: %t", tg->file,
                               tv->tc_id, tv->comment, tv->want);
        arena_reset(&ar);
    }

    arena_free(&keys);
    arena_free(&ar);
}

/* ------------------------------------------------- the FIPS module's own */

typedef struct FipsCurve {
    const char *name;
    const EcdsaFipsCurve *(*curve)(void);
} FipsCurve;

static const FipsCurve fips_curves[] = {
    {"P-224", burrow__ecdsa_fips_p224},
    {"P-256", burrow__ecdsa_fips_p256},
    {"P-384", burrow__ecdsa_fips_p384},
    {"P-521", burrow__ecdsa_fips_p521},
};

/* What testRandomPoint hands randomPoint: one Read of r, whatever it gives. */
static bool fill_from(void *env, Slice b, Error *err) {
    IoReader *r = env;
    Error e = BURROW_NO_ERROR;
    (void)r->vt->read(r->data, b, &e);
    if (BURROW_FAILED(e)) {
        *err = e;
        return false;
    }
    return true;
}

static bool all_zero(const uint8_t *p, Int n) {
    uint8_t acc = 0;
    for (Int i = 0; i < n; i++)
        acc |= p[i];
    return acc == 0;
}

/* One randomPoint over r, with the checks testRandomPoint makes of what comes
 * back. The number of rejections is returned. */
static Int random_point_check(TestingT *t, const EcdsaFipsCurve *c, IoReader r) {
    uint8_t k[NISTEC_MAX_ELEMENT_BYTES], p[NISTEC_MAX_POINT_BYTES];
    Int loops = 0;
    Error err = BURROW_NO_ERROR;
    if (!burrow__ecdsa_random_point(c, BURROW_FN(EcdsaFill, fill_from, &r), k, p,
                                    &loops, &err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    else if (all_zero(k, burrow__ecdsa_fips_size(c)))
        testing_t_errorf_v(t, "k is zero");
    else if (p[0] != 4)
        testing_t_errorf_v(t, "p is infinity");
    return loops;
}

static void random_point(void *env, TestingT *t) {
    const FipsCurve *fc = env;
    const EcdsaFipsCurve *c = fc->curve();
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    uint8_t filler[100];

    /* A sequence of all ones will generate 2^N-1, which should be rejected.
     * (Unless, for example, we are masking too many bits.) */
    memset(filler, 0xff, sizeof filler);
    BytesReader br;
    bytes_reader_reset(&br, bs(filler, sizeof filler));
    IoReader readers[2] = {bytes_reader_as_io_reader(&br), crypto_rand_reader};
    if (random_point_check(t, c, io_multi_reader(a, readers, 2)) == 0)
        testing_t_errorf_v(t, "overflow was not rejected");

    /* A sequence of all zeroes will generate zero, which should be rejected. */
    memset(filler, 0, sizeof filler);
    bytes_reader_reset(&br, bs(filler, sizeof filler));
    if (random_point_check(t, c, io_multi_reader(a, readers, 2)) == 0)
        testing_t_errorf_v(t, "zero was not rejected");

    /* P-256 has a 2⁻³² chance of randomly hitting a rejection. For P-224 it's
     * 2⁻¹¹², for P-384 it's 2⁻¹⁹⁴, and for P-521 it's 2⁻²⁶², so if we hit in
     * tests, something is horribly wrong. (For example, we are masking the
     * wrong bits.) */
    if (c != burrow__ecdsa_fips_p256() &&
        random_point_check(t, c, crypto_rand_reader) > 0)
        testing_t_errorf_v(t, "unexpected rejection");

    arena_free(&ar);
}

static void TestRandomPoint(TestingT *t) {
    for (size_t i = 0; i < sizeof fips_curves / sizeof fips_curves[0]; i++)
        testing_t_run(
            t, str_from_cstr(fips_curves[i].name),
            BURROW_FN(TestingTFunc, random_point, (void *)(uintptr_t)&fips_curves[i]));
}

static void hash_to_nat(void *env, TestingT *t) {
    (void)t;
    const EcdsaFipsCurve *c = ((const FipsCurve *)env)->curve();
    uint8_t h[600], out[NISTEC_MAX_ELEMENT_BYTES];
    memset(h, 0xff, sizeof h);
    for (Int l = 0; l < 600; l++)
        burrow__ecdsa_bits2octets(c, bs(h, l), out);
}

static void TestHashToNat(TestingT *t) {
    for (size_t i = 0; i < sizeof fips_curves / sizeof fips_curves[0]; i++)
        testing_t_run(
            t, str_from_cstr(fips_curves[i].name),
            BURROW_FN(TestingTFunc, hash_to_nat, (void *)(uintptr_t)&fips_curves[i]));
}

/* testPrivateKey, from RFC 9500, section 2.3, and testHash. */
static const uint8_t cast_q[] = {
    0x04, 0x42, 0x25, 0x48, 0xF8, 0x8F, 0xB7, 0x82, 0xFF, 0xB5, 0xEC, 0xA3, 0x74,
    0x44, 0x52, 0xC7, 0x2A, 0x1E, 0x55, 0x8F, 0xBD, 0x6F, 0x73, 0xBE, 0x5E, 0x48,
    0xE9, 0x32, 0x32, 0xCC, 0x45, 0xC5, 0xB1, 0x6C, 0x4C, 0xD1, 0x0C, 0x4C, 0xB8,
    0xD5, 0xB8, 0xA1, 0x71, 0x39, 0xE9, 0x48, 0x82, 0xC8, 0x99, 0x25, 0x72, 0x99,
    0x34, 0x25, 0xF4, 0x14, 0x19, 0xAB, 0x7E, 0x90, 0xA4, 0x2A, 0x49, 0x42, 0x72,
};

static const uint8_t cast_d[] = {
    0xE6, 0xCB, 0x5B, 0xDD, 0x80, 0xAA, 0x45, 0xAE, 0x9C, 0x95, 0xE8,
    0xC1, 0x54, 0x76, 0x67, 0x9F, 0xFE, 0xC9, 0x53, 0xC1, 0x68, 0x51,
    0xE7, 0x11, 0xE7, 0x43, 0x93, 0x95, 0x89, 0xC6, 0x4F, 0xC1,
};

static const uint8_t cast_hash[] = {
    0x17, 0x1b, 0x1f, 0x5e, 0x9f, 0x8f, 0x8c, 0x5c, 0x42, 0xe8, 0x06, 0x59, 0x7b,
    0x54, 0xc7, 0xb4, 0x49, 0x05, 0xa1, 0xdb, 0x3a, 0x3c, 0x31, 0xd3, 0xb7, 0x56,
    0x45, 0x8c, 0xc2, 0xd6, 0x88, 0x62, 0x9e, 0xd6, 0x7b, 0x9b, 0x25, 0x68, 0xd6,
    0xc6, 0x18, 0x94, 0x1e, 0xfe, 0xe3, 0x33, 0x78, 0xa6, 0xe1, 0xce, 0x13, 0x88,
    0x81, 0x26, 0x02, 0x52, 0xdf, 0xc2, 0x0a, 0xf2, 0x67, 0x49, 0x0a, 0x20,
};

/* sign with drbg over testPrivateKey and testHash, then verify, which the
 * CAST does through the FIPS module's verify and this through ecdsa_verify,
 * which is the same thing for P-256, and the result against want. */
static void cast_check(TestingT *t, Alloc *a, EcdsaDrbg *drbg, const uint8_t *want_r,
                       const uint8_t *want_s) {
    const EcdsaFipsCurve *c = burrow__ecdsa_fips_p256();
    uint8_t r[32], s[32];
    Error err = BURROW_NO_ERROR;
    if (!burrow__ecdsa_sign_with_drbg(c, bs(cast_d, sizeof cast_d), drbg,
                                      bs(cast_hash, sizeof cast_hash), r, s, &err))
        testing_t_fatalf_v(t, "%s", error_text(err));

    EcdsaPublicKey *pub = ecdsa_parse_uncompressed_public_key(
        a, elliptic_p256(), bs(cast_q, sizeof cast_q), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    BigInt *br = big_new_int(a, 0), *bsv = big_new_int(a, 0);
    big_int_set_bytes(br, bs(r, 32));
    big_int_set_bytes(bsv, bs(s, 32));
    if (!ecdsa_verify(pub, bs(cast_hash, sizeof cast_hash), br, bsv))
        testing_t_errorf_v(t, "signature does not verify");

    if (memcmp(r, want_r, 32) != 0 || memcmp(s, want_s, 32) != 0)
        testing_t_errorf_v(t, "unexpected result: r = %s, s = %s", hexs(a, bs(r, 32)),
                           hexs(a, bs(s, 32)));
}

/* "ECDSA P-256 SHA2-512 sign and verify". */
static void TestCASTSign(TestingT *t) {
    static const uint8_t z[] = {
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
        0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10,
    };
    static const uint8_t pers_str[] = {
        0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
        0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20,
    };
    static const uint8_t want_r[] = {
        0x33, 0x64, 0x96, 0xff, 0x8a, 0xfe, 0xaa, 0x0b, 0x2c, 0x4a, 0x1a,
        0x97, 0x77, 0xcc, 0x84, 0xa5, 0x7e, 0x88, 0x1f, 0x16, 0x2d, 0xe0,
        0x29, 0xf7, 0x62, 0xc2, 0x34, 0x18, 0x10, 0x9c, 0x69, 0x8a,
    };
    static const uint8_t want_s[] = {
        0x97, 0x53, 0x2e, 0x13, 0x6e, 0xd0, 0x9b, 0x30, 0x8a, 0xdf, 0x4f,
        0xe0, 0x54, 0x82, 0x14, 0x83, 0x5e, 0x93, 0xc7, 0x79, 0x4b, 0x18,
        0xa3, 0xf1, 0x8a, 0x60, 0xae, 0x52, 0x31, 0xe4, 0x2e, 0x4e,
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    EcdsaDrbg *drbg = burrow__ecdsa_new_drbg(a, sha512_new, bs(z, sizeof z), (Slice){0},
                                             bs(pers_str, sizeof pers_str));
    cast_check(t, a, drbg, want_r, want_s);
    arena_free(&ar);
}

/* "DetECDSA P-256 SHA2-512 sign". */
static void TestCASTSignDeterministic(TestingT *t) {
    static const uint8_t want_r[] = {
        0x9f, 0xc3, 0x83, 0x32, 0x6e, 0xd9, 0x4f, 0x8e, 0x24, 0xa0, 0x19,
        0xef, 0x1d, 0x3a, 0xc3, 0x55, 0xdd, 0x4b, 0x98, 0xae, 0x78, 0xa7,
        0xaf, 0xd3, 0xfd, 0xf3, 0x22, 0x1c, 0x8b, 0xd6, 0x11, 0x7b,
    };
    static const uint8_t want_s[] = {
        0xd6, 0x52, 0x87, 0x41, 0x71, 0xbd, 0x66, 0xd1, 0xaf, 0x6c, 0x61,
        0xdd, 0xd8, 0xa7, 0xbb, 0xd2, 0xf7, 0xd5, 0x47, 0x70, 0xe9, 0xe4,
        0xac, 0x0a, 0xb9, 0xfa, 0x0f, 0xbd, 0x3b, 0x9b, 0xc2, 0xfe,
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    uint8_t nonce[32];
    burrow__ecdsa_bits2octets(burrow__ecdsa_fips_p256(),
                              bs(cast_hash, sizeof cast_hash), nonce);
    EcdsaDrbg *drbg = burrow__ecdsa_new_drbg(a, sha512_new, bs(cast_d, sizeof cast_d),
                                             bs(nonce, sizeof nonce), (Slice){0});
    cast_check(t, a, drbg, want_r, want_s);
    arena_free(&ar);
}

/* ------------------------------------------------------------- HeapKeys */

/* Keys, signatures and integers outside an arena, to check that each frees
 * cleanly, which the sanitizer builds look at. */
static void TestHeapKeys(TestingT *t) {
    Alloc *a = heap_allocator();
    EllipticCurve cs4[] = {elliptic_p224(), elliptic_p256(), elliptic_p384(),
                           elliptic_p521()};
    for (size_t i = 0; i < sizeof cs4 / sizeof cs4[0]; i++) {
        Error err = BURROW_NO_ERROR;
        EcdsaPrivateKey *k = ecdsa_generate_key(a, cs4[i], crypto_rand_reader, &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "%s", error_text(err));
        Slice h = cs("testing");
        Slice sig = ecdsa_sign_asn1(a, crypto_rand_reader, k, h, &err);
        BigInt *s = NULL;
        BigInt *r = ecdsa_sign(a, crypto_rand_reader, k, h, &s, &err);
        Slice b = ecdsa_public_key_bytes(&k->public_key, a, &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "%s", error_text(err));
        EcdsaPublicKey *p = ecdsa_parse_uncompressed_public_key(a, cs4[i], b, &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "%s", error_text(err));
        if (!ecdsa_verify_asn1(p, h, sig) || !ecdsa_verify(p, h, r, s))
            testing_t_errorf_v(t, "signature does not verify");
        big_int_free(r);
        big_int_free(s);
        mem_free(a, r, sizeof *r, _Alignof(BigInt));
        mem_free(a, s, sizeof *s, _Alignof(BigInt));
        mem_free(a, sig.p, (size_t)sig.cap, 1);
        mem_free(a, b.p, (size_t)b.cap, 1);
        ecdsa_public_key_free(p);
        ecdsa_private_key_free(k);
    }
    ecdsa_private_key_free(NULL);
    ecdsa_public_key_free(NULL);
}

/* ----------------------------------------------------------- benchmarks */

typedef void (*CurveBench)(TestingB *b, EllipticCurve c);

typedef struct CurveBenchEnv {
    CurveBench f;
    EllipticCurve c;
} CurveBenchEnv;

static void curve_bench_run(void *env, TestingB *b) {
    const CurveBenchEnv *e = env;
    e->f(b, e->c);
}

static void bench_all_curves(TestingB *b, CurveBench f) {
    const struct {
        const char *name;
        EllipticCurve c;
    } tests[] = {
        {"P256", elliptic_p256()},
        {"P384", elliptic_p384()},
        {"P521", elliptic_p521()},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        CurveBenchEnv e = {f, tests[i].c};
        testing_b_run(b, str_from_cstr(tests[i].name),
                      BURROW_FN(TestingBFunc, curve_bench_run, &e));
    }
}

static void bench_sign(TestingB *b, EllipticCurve c) {
    Arena keys, ar;
    arena_init(&keys, NULL, 0);
    arena_init(&ar, NULL, 0);
    Alloc *ka = arena_allocator(&keys);
    Alloc *a = arena_allocator(&ar);
    IoReader r = bufio_reader_as_io_reader(
        bufio_new_reader_size(ka, crypto_rand_reader, 1 << 15));
    Error err = BURROW_NO_ERROR;
    EcdsaPrivateKey *priv = ecdsa_generate_key(ka, c, r, &err);
    if (BURROW_FAILED(err))
        testing_b_fatalf_v(b, "%s", error_text(err));
    uint8_t hashed[] = "testing";

    testing_b_report_allocs(b);
    testing_b_reset_timer(b);
    for (Int i = 0; i < testing_b_n(b); i++) {
        Slice sig = ecdsa_sign_asn1(a, r, priv, bs(hashed, 7), &err);
        if (BURROW_FAILED(err))
            testing_b_fatalf_v(b, "%s", error_text(err));
        /* Prevent the compiler from optimizing out the operation. */
        hashed[0] = ((const uint8_t *)sig.p)[0];
        arena_reset(&ar);
    }
    arena_free(&ar);
    arena_free(&keys);
}

static void BenchmarkSign(TestingB *b) {
    bench_all_curves(b, bench_sign);
}

static void bench_verify(TestingB *b, EllipticCurve c) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    IoReader r = bufio_reader_as_io_reader(
        bufio_new_reader_size(a, crypto_rand_reader, 1 << 15));
    Error err = BURROW_NO_ERROR;
    EcdsaPrivateKey *priv = ecdsa_generate_key(a, c, r, &err);
    if (BURROW_FAILED(err))
        testing_b_fatalf_v(b, "%s", error_text(err));
    Slice hashed = cs("testing");
    Slice sig = ecdsa_sign_asn1(a, r, priv, hashed, &err);
    if (BURROW_FAILED(err))
        testing_b_fatalf_v(b, "%s", error_text(err));

    testing_b_report_allocs(b);
    testing_b_reset_timer(b);
    for (Int i = 0; i < testing_b_n(b); i++)
        if (!ecdsa_verify_asn1(&priv->public_key, hashed, sig))
            testing_b_fatalf_v(b, "verify failed");
    arena_free(&ar);
}

static void BenchmarkVerify(TestingB *b) {
    bench_all_curves(b, bench_verify);
}

static void bench_generate_key(TestingB *b, EllipticCurve c) {
    Arena keys, ar;
    arena_init(&keys, NULL, 0);
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    IoReader r = bufio_reader_as_io_reader(
        bufio_new_reader_size(arena_allocator(&keys), crypto_rand_reader, 1 << 15));

    testing_b_report_allocs(b);
    testing_b_reset_timer(b);
    for (Int i = 0; i < testing_b_n(b); i++) {
        Error err = BURROW_NO_ERROR;
        (void)ecdsa_generate_key(a, c, r, &err);
        if (BURROW_FAILED(err))
            testing_b_fatalf_v(b, "%s", error_text(err));
        arena_reset(&ar);
    }
    arena_free(&ar);
    arena_free(&keys);
}

static void BenchmarkGenerateKey(TestingB *b) {
    bench_all_curves(b, bench_generate_key);
}

#define TESTS(X)                                                                       \
    X(TestKeyGeneration)                                                               \
    X(TestSignAndVerify)                                                               \
    X(TestSignAndVerifyASN1)                                                           \
    X(TestEmptyHashRejection)                                                          \
    X(TestSignHashLength)                                                              \
    X(TestNonceSafety)                                                                 \
    X(TestINDCCA)                                                                      \
    X(TestVectors)                                                                     \
    X(TestNegativeInputs)                                                              \
    X(TestZeroHashSignature)                                                           \
    X(TestZeroSignature)                                                               \
    X(TestNegativeSignature)                                                           \
    X(TestRPlusNSignature)                                                             \
    X(TestRMinusNSignature)                                                            \
    X(TestRFC6979)                                                                     \
    X(TestParseAndBytesRoundTrip)                                                      \
    X(TestInvalidPublicKeys)                                                           \
    X(TestInvalidPrivateKeys)                                                          \
    X(TestKeyGenerationVectors)                                                        \
    X(TestEqual)                                                                       \
    X(TestECDSAWycheproof)                                                             \
    X(TestRandomPoint)                                                                 \
    X(TestHashToNat)                                                                   \
    X(TestCASTSign)                                                                    \
    X(TestCASTSignDeterministic)                                                       \
    X(TestHeapKeys)                                                                    \
    X(BenchmarkSign)                                                                   \
    X(BenchmarkVerify)                                                                 \
    X(BenchmarkGenerateKey)

TESTING_MAIN(TESTS)
