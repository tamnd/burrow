/* Derived from Go's src/crypto/ed25519/ed25519_test.go, ed25519vectors_test.go
 * and ed25519_wycheproof_test.go.
 * Go source: go1.27.1.
 *
 * Go's tests read testdata/sign.input.gz and fetch two modules of vectors when
 * they run. All three are in ed25519_test_gen.h, which
 * tools/gen-ed25519-tests.sh writes, and the Wycheproof keys are already out of
 * their DER there, as Go's x509 would have taken them, since there is no x509
 * here yet. The generator also checks that Go's own Verify gives what each test
 * expects, so a vector that fails here is not one Go fails too.
 *
 * TestAllocations has nothing to count: Verify takes no allocator, and Sign and
 * Public take one for the slice they return and allocate nothing else.
 *
 * Copyright 2016 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/bufio.h"
#include "burrow/burrow.h"
#include "burrow/bytes.h"
#include "burrow/compress/gzip.h"
#include "burrow/crypto.h"
#include "burrow/crypto/ed25519.h"
#include "burrow/crypto/rand.h"
#include "burrow/crypto/sha512.h"
#include "burrow/encoding/hex.h"
#include "burrow/strings.h"

#include "ed25519_test_gen.h"

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

static const Type test_type = {
    {(const Byte *)"zeroReader", 10},
    {(const Byte *)"ed25519_test", 12},
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
    0x65643235U,
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

static IoReader nil_reader(void) {
    IoReader r = {NULL, NULL};
    return r;
}

static void TestGenerateKey(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;

    /* nil is like using crypto/rand.Reader. */
    Ed25519PrivateKey priv;
    Ed25519PublicKey pub = ed25519_generate_key(a, nil_reader(), &priv, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));

    if (pub.len != ED25519_PUBLIC_KEY_SIZE)
        testing_t_errorf_v(t, "public key has the wrong size: %d", pub.len);
    if (priv.len != ED25519_PRIVATE_KEY_SIZE)
        testing_t_errorf_v(t, "private key has the wrong size: %d", priv.len);
    if (!bytes_equal(ed25519_private_key_public(priv, a), pub))
        testing_t_errorf_v(t, "public key doesn't match private key");
    Ed25519PrivateKey from_seed =
        ed25519_new_key_from_seed(a, ed25519_private_key_seed(priv, a));
    if (!bytes_equal(priv, from_seed))
        testing_t_errorf_v(t,
                           "recreating key pair from seed gave different private key");

    Ed25519PrivateKey k2;
    ed25519_generate_key(a, nil_reader(), &k2, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    if (bytes_equal(priv, k2))
        testing_t_errorf_v(t, "GenerateKey returned the same private key twice");

    Ed25519PrivateKey k3;
    ed25519_generate_key(a, crypto_rand_reader, &k3, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    if (bytes_equal(priv, k3))
        testing_t_errorf_v(t, "GenerateKey returned the same private key twice");

    /* GenerateKey is documented to be the same as NewKeyFromSeed. */
    uint8_t seed[ED25519_SEED_SIZE];
    crypto_rand_read(bs(seed, sizeof seed), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    BytesReader r;
    bytes_reader_reset(&r, bs(seed, sizeof seed));
    Ed25519PrivateKey k4;
    ed25519_generate_key(a, bytes_reader_as_io_reader(&r), &k4, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    Ed25519PrivateKey k4n = ed25519_new_key_from_seed(a, bs(seed, sizeof seed));
    if (!bytes_equal(k4, k4n))
        testing_t_errorf_v(t, "GenerateKey with seed gave different private key");

    arena_free(&ar);
}

static void TestSignVerify(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;

    Ed25519PrivateKey priv;
    Ed25519PublicKey pub = ed25519_generate_key(a, zero_reader(), &priv, &err);

    Slice message = cs("test message");
    Slice sig = ed25519_sign(a, priv, message);
    if (!ed25519_verify(pub, message, sig))
        testing_t_errorf_v(t, "valid signature rejected");

    Slice wrong_message = cs("wrong message");
    if (ed25519_verify(pub, wrong_message, sig))
        testing_t_errorf_v(t, "signature of different message accepted");

    arena_free(&ar);
}

static void TestSignVerifyHashed(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;

    /* From RFC 8032, Section 7.3 */
    Slice key =
        unhex(a, "833fe62409237b9d62ec77587520911e9a759cec1d19755b7da901b96dca3d42"
                 "ec172b93ad5e563bf4932c70e1245034c35467ef2efd4d64ebf819683467e2bf");
    Slice expected_sig =
        unhex(a, "98a70222f0b8121aa9d30f813d683f809e462b469c7ff87639499bb94e6dae41"
                 "31f85042463c2a355a2003d062adf5aaa10b8c61e636062aaad11c2a26083406");
    Slice message = unhex(a, "616263");

    Ed25519PrivateKey priv = key;
    Ed25519PublicKey pub = ed25519_private_key_public(priv, a);
    Sha512Sum512Ret sum = sha512_sum512(message);
    Slice hash = bs(sum.a, sizeof sum.a);
    CryptoHash sha512 = CRYPTO_SHA512;
    Slice sig = ed25519_private_key_sign(priv, a, nil_reader(), hash,
                                         crypto_hash_as_signer_opts(&sha512), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    if (!bytes_equal(sig, expected_sig))
        testing_t_errorf_v(t, "signature doesn't match test vector");
    Ed25519Options ph = {CRYPTO_SHA512, BURROW_S("")};
    sig = ed25519_private_key_sign(priv, a, nil_reader(), hash,
                                   ed25519_options_as_signer_opts(&ph), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    if (!bytes_equal(sig, expected_sig))
        testing_t_errorf_v(t, "signature doesn't match test vector");
    err = ed25519_verify_with_options(pub, hash, sig, &ph);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "valid signature rejected: %s", error_text(err));

    Ed25519Options wrong = {CRYPTO_SHA256, BURROW_S("")};
    if (BURROW_OK(ed25519_verify_with_options(pub, hash, sig, &wrong)))
        testing_t_errorf_v(t, "expected error for wrong hash");

    Sha512Sum512Ret wrong_sum = sha512_sum512(cs("wrong message"));
    if (BURROW_OK(ed25519_verify_with_options(pub, bs(wrong_sum.a, sizeof wrong_sum.a),
                                              sig, &ph)))
        testing_t_errorf_v(t, "signature of different message accepted");

    Byte *s = sig.p;
    s[0] ^= 0xff;
    if (BURROW_OK(ed25519_verify_with_options(pub, hash, sig, &ph)))
        testing_t_errorf_v(t, "invalid signature accepted");
    s[0] ^= 0xff;
    s[ED25519_SIGNATURE_SIZE - 1] ^= 0xff;
    if (BURROW_OK(ed25519_verify_with_options(pub, hash, sig, &ph)))
        testing_t_errorf_v(t, "invalid signature accepted");

    /* The RFC provides no test vectors for Ed25519ph with context, so just sign
     * and verify something. */
    Ed25519Options ph123 = {CRYPTO_SHA512, BURROW_S("123")};
    sig = ed25519_private_key_sign(priv, a, nil_reader(), hash,
                                   ed25519_options_as_signer_opts(&ph123), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    err = ed25519_verify_with_options(pub, hash, sig, &ph123);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "valid signature rejected: %s", error_text(err));
    Ed25519Options ph321 = {CRYPTO_SHA512, BURROW_S("321")};
    if (BURROW_OK(ed25519_verify_with_options(pub, hash, sig, &ph321)))
        testing_t_errorf_v(t, "expected error for wrong context");
    Ed25519Options sha256_123 = {CRYPTO_SHA256, BURROW_S("123")};
    if (BURROW_OK(ed25519_verify_with_options(pub, hash, sig, &sha256_123)))
        testing_t_errorf_v(t, "expected error for wrong hash");

    arena_free(&ar);
}

static void TestSignVerifyContext(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;

    /* From RFC 8032, Section 7.2 */
    Slice key =
        unhex(a, "0305334e381af78f141cb666f6199f57bc3495335a256a95bd2a55bf546663f6"
                 "dfc9425e4f968f7f0c29f0259cf5f9aed6851c2bb4ad8bfb860cfee0ab248292");
    Slice expected_sig =
        unhex(a, "55a4cc2f70a54e04288c5f4cd1e45a7bb520b36292911876cada7323198dd87a"
                 "8b36950b95130022907a7fb7c4e9b2d5f6cca685a587b4b21f4b888e4e7edb0d");
    Slice message = unhex(a, "f726936d19c800494e3fdaff20b276a8");
    Ed25519Options opts = {0, BURROW_S("foo")};

    Ed25519PrivateKey priv = key;
    Ed25519PublicKey pub = ed25519_private_key_public(priv, a);
    Slice sig = ed25519_private_key_sign(priv, a, nil_reader(), message,
                                         ed25519_options_as_signer_opts(&opts), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    if (!bytes_equal(sig, expected_sig))
        testing_t_errorf_v(t, "signature doesn't match test vector");
    err = ed25519_verify_with_options(pub, message, sig, &opts);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "valid signature rejected: %s", error_text(err));

    if (BURROW_OK(ed25519_verify_with_options(pub, cs("bar"), sig, &opts)))
        testing_t_errorf_v(t, "signature of different message accepted");
    Ed25519Options bar = {0, BURROW_S("bar")};
    if (BURROW_OK(ed25519_verify_with_options(pub, message, sig, &bar)))
        testing_t_errorf_v(t, "signature with different context accepted");

    Byte *s = sig.p;
    s[0] ^= 0xff;
    if (BURROW_OK(ed25519_verify_with_options(pub, message, sig, &opts)))
        testing_t_errorf_v(t, "invalid signature accepted");
    s[0] ^= 0xff;
    s[ED25519_SIGNATURE_SIZE - 1] ^= 0xff;
    if (BURROW_OK(ed25519_verify_with_options(pub, message, sig, &opts)))
        testing_t_errorf_v(t, "invalid signature accepted");

    arena_free(&ar);
}

static void TestCryptoSigner(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;

    Ed25519PrivateKey priv;
    Ed25519PublicKey pub = ed25519_generate_key(a, zero_reader(), &priv, &err);

    Ed25519Signer holder;
    CryptoSigner signer = ed25519_private_key_signer(priv, &holder);

    CryptoPublicKey public_interface = crypto_signer_public(signer);
    if (public_interface.t != TYPE_ED25519_PUBLIC_KEY)
        testing_t_fatalf_v(t, "expected PublicKey from Public() but got another type");
    Ed25519PublicKey pub2 = *(const Slice *)public_interface.data;

    if (!bytes_equal(pub, pub2))
        testing_t_errorf_v(t, "public keys do not match");

    Slice message = cs("message");
    CryptoHash no_hash = 0;
    Slice signature = crypto_signer_sign(signer, a, zero_reader(), message,
                                         crypto_hash_as_signer_opts(&no_hash), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "error from Sign(): %s", error_text(err));

    Ed25519Options opts = {no_hash, BURROW_S("")};
    Slice signature2 = crypto_signer_sign(signer, a, zero_reader(), message,
                                          ed25519_options_as_signer_opts(&opts), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "error from Sign(): %s", error_text(err));
    if (!bytes_equal(signature, signature2))
        testing_t_errorf_v(t, "signatures keys do not match");

    if (!ed25519_verify(pub, message, signature))
        testing_t_errorf_v(t, "Verify failed on signature from Sign()");

    arena_free(&ar);
}

static void TestEqual(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;

    Ed25519PrivateKey priv;
    Ed25519PublicKey pub = ed25519_generate_key(a, crypto_rand_reader, &priv, &err);

    if (!ed25519_public_key_equal(pub, BURROW_ANY(TYPE_ED25519_PUBLIC_KEY, &pub)))
        testing_t_errorf_v(t, "public key is not equal to itself");
    Ed25519Signer holder;
    CryptoSigner signer = ed25519_private_key_signer(priv, &holder);
    if (!ed25519_public_key_equal(pub, crypto_signer_public(signer)))
        testing_t_errorf_v(t, "private.Public() is not Equal to public");
    if (!ed25519_private_key_equal(priv, BURROW_ANY(TYPE_ED25519_PRIVATE_KEY, &priv)))
        testing_t_errorf_v(t, "private key is not equal to itself");

    Ed25519PrivateKey other_priv;
    Ed25519PublicKey other_pub =
        ed25519_generate_key(a, crypto_rand_reader, &other_priv, &err);
    if (ed25519_public_key_equal(pub, BURROW_ANY(TYPE_ED25519_PUBLIC_KEY, &other_pub)))
        testing_t_errorf_v(t, "different public keys are Equal");
    if (ed25519_private_key_equal(priv,
                                  BURROW_ANY(TYPE_ED25519_PRIVATE_KEY, &other_priv)))
        testing_t_errorf_v(t, "different private keys are Equal");

    arena_free(&ar);
}

static void TestGolden(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;

    /* sign.input.gz is a selection of test cases from
     * https://ed25519.cr.yp.to/python/sign.input */
    BytesReader zr;
    bytes_reader_reset(&zr, bs(gen_sign_input_gz, sizeof gen_sign_input_gz));
    GzipReader *test_data = gzip_new_reader(a, bytes_reader_as_io_reader(&zr), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));

    BufioScanner *scanner = bufio_new_scanner(a, gzip_reader_as_io_reader(test_data));
    int line_no = 0;

    while (bufio_scanner_scan(scanner)) {
        line_no++;

        Str line = bufio_scanner_text(scanner);
        Slice parts = strings_split(a, line, BURROW_S(":"));
        if (parts.len != 5)
            testing_t_fatalf_v(t, "bad number of parts on line %d", line_no);
        const Str *p = parts.p;

        Slice priv_bytes = hex_decode_string(a, p[0], &err);
        Slice pub_key = hex_decode_string(a, p[1], &err);
        Slice msg = hex_decode_string(a, p[2], &err);
        Slice sig = hex_decode_string(a, p[3], &err);
        /* The signatures in the test vectors also include the message at the
         * end, but we just want R and S. */
        sig.len = ED25519_SIGNATURE_SIZE;

        if (pub_key.len != ED25519_PUBLIC_KEY_SIZE)
            testing_t_fatalf_v(t, "bad public key length on line %d: got %d bytes",
                               line_no, pub_key.len);

        uint8_t priv[ED25519_PRIVATE_KEY_SIZE] = {0};
        memcpy(priv, priv_bytes.p, (size_t)(priv_bytes.len < 64 ? priv_bytes.len : 64));
        memcpy(priv + 32, pub_key.p, 32);

        Slice sig2 = ed25519_sign(a, bs(priv, sizeof priv), msg);
        if (!bytes_equal(sig, sig2))
            testing_t_errorf_v(t, "different signature result on line %d", line_no);

        if (!ed25519_verify(pub_key, msg, sig2))
            testing_t_errorf_v(t, "signature failed to verify on line %d", line_no);

        Ed25519PrivateKey priv2 = ed25519_new_key_from_seed(a, bs(priv, 32));
        if (!bytes_equal(bs(priv, sizeof priv), priv2))
            testing_t_errorf_v(
                t, "recreating key pair gave different private key on line %d",
                line_no);

        if (!bytes_equal(pub_key, ed25519_private_key_public(priv2, a)))
            testing_t_errorf_v(
                t, "recreating key pair gave different public key on line %d", line_no);

        if (!bytes_equal(bs(priv, 32), ed25519_private_key_seed(priv2, a)))
            testing_t_errorf_v(t, "recreating key pair gave different seed on line %d",
                               line_no);
    }

    err = bufio_scanner_err(scanner);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "error reading test data: %s", error_text(err));
    if (line_no == 0)
        testing_t_fatalf_v(t, "no test data");

    arena_free(&ar);
}

static void TestMalleability(TestingT *t) {
    /* https://tools.ietf.org/html/rfc8032#section-5.1.7 adds an additional test
     * that s be in [0, order). This prevents someone from adding a multiple of
     * order to s and obtaining a second valid signature for the same message. */
    static const uint8_t msg[] = {0x54, 0x65, 0x73, 0x74};
    static const uint8_t sig[] = {
        0x7c, 0x38, 0xe0, 0x26, 0xf2, 0x9e, 0x14, 0xaa, 0xbd, 0x05, 0x9a, 0x0f, 0x2d,
        0xb8, 0xb0, 0xcd, 0x78, 0x30, 0x40, 0x60, 0x9a, 0x8b, 0xe6, 0x84, 0xdb, 0x12,
        0xf8, 0x2a, 0x27, 0x77, 0x4a, 0xb0, 0x67, 0x65, 0x4b, 0xce, 0x38, 0x32, 0xc2,
        0xd7, 0x6f, 0x8f, 0x6f, 0x5d, 0xaf, 0xc0, 0x8d, 0x93, 0x39, 0xd4, 0xee, 0xf6,
        0x76, 0x57, 0x33, 0x36, 0xa5, 0xc5, 0x1e, 0xb6, 0xf9, 0x46, 0xb3, 0x1d,
    };
    static const uint8_t public_key[] = {
        0x7d, 0x4d, 0x0e, 0x7f, 0x61, 0x53, 0xa6, 0x9b, 0x62, 0x42, 0xb5,
        0x22, 0xab, 0xbe, 0xe6, 0x85, 0xfd, 0xa4, 0x42, 0x0f, 0x88, 0x34,
        0xb1, 0x08, 0xc3, 0xbd, 0xae, 0x36, 0x9e, 0xf5, 0x49, 0xfa,
    };

    if (ed25519_verify(bs(public_key, sizeof public_key), bs(msg, sizeof msg),
                       bs(sig, sizeof sig)))
        testing_t_fatalf_v(t, "non-canonical signature accepted");
}

static void TestEd25519Vectors(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    for (size_t i = 0; i < sizeof gen_vectors / sizeof gen_vectors[0]; i++) {
        const GenVector *v = &gen_vectors[i];
        /* We use the simplified verification formula that doesn't multiply by
         * the cofactor, so any low order residue will cause the signature not
         * to verify. Our point decoding allows non-canonical encodings (in
         * violation of RFC 8032) but R is not decoded: instead, R is recomputed
         * and compared bytewise against the canonical encoding. The generator
         * has already worked out want from the flags that way. */
        bool expected_to_verify = v->want;

        Slice public_key = unhex(a, v->a);
        Slice r = unhex(a, v->r);
        Slice s = unhex(a, v->s);
        uint8_t signature[ED25519_SIGNATURE_SIZE + 1];
        if (r.len + s.len > (Int)sizeof signature)
            testing_t_fatalf_v(t, "#%d: signature too long", (int)i);
        memcpy(signature, r.p, (size_t)r.len);
        memcpy(signature + r.len, s.p, (size_t)s.len);
        Slice message = cs(v->m);

        bool did_verify =
            ed25519_verify(public_key, message, bs(signature, r.len + s.len));
        if (did_verify && !expected_to_verify)
            testing_t_errorf_v(t, "#%d: vector with flags [%s] unexpectedly verified",
                               (int)i, v->flags);
        if (!did_verify && expected_to_verify)
            testing_t_errorf_v(t, "#%d: vector with flags [%s] unexpectedly rejected",
                               (int)i, v->flags);
    }

    arena_free(&ar);
}

static void TestEd25519Wycheproof(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    for (size_t i = 0; i < sizeof gen_wycheproof / sizeof gen_wycheproof[0]; i++) {
        const GenWycheproof *tv = &gen_wycheproof[i];
        bool got =
            ed25519_verify(unhex(a, tv->pub), unhex(a, tv->msg), unhex(a, tv->sig));
        if (got != tv->want)
            testing_t_errorf_v(t, "ed25519_test.json/%d: %s: Verify wanted success: %t",
                               tv->tc_id, tv->comment, tv->want);
    }

    arena_free(&ar);
}

static void BenchmarkKeyGeneration(TestingB *b) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < testing_b_n(b); i++) {
        Error err = BURROW_NO_ERROR;
        Ed25519PrivateKey priv;
        ed25519_generate_key(a, zero_reader(), &priv, &err);
        if (BURROW_FAILED(err))
            testing_b_fatalf_v(b, "%s", error_text(err));
        arena_reset(&ar);
    }
    arena_free(&ar);
}

static void BenchmarkNewKeyFromSeed(TestingB *b) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    uint8_t seed[ED25519_SEED_SIZE] = {0};
    for (Int i = 0; i < testing_b_n(b); i++) {
        (void)ed25519_new_key_from_seed(a, bs(seed, sizeof seed));
        arena_reset(&ar);
    }
    arena_free(&ar);
}

static void BenchmarkSigning(TestingB *b) {
    Arena keys, ar;
    arena_init(&keys, NULL, 0);
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    Ed25519PrivateKey priv;
    ed25519_generate_key(arena_allocator(&keys), zero_reader(), &priv, &err);
    if (BURROW_FAILED(err))
        testing_b_fatalf_v(b, "%s", error_text(err));
    Slice message = cs("Hello, world!");
    testing_b_reset_timer(b);
    for (Int i = 0; i < testing_b_n(b); i++) {
        (void)ed25519_sign(a, priv, message);
        arena_reset(&ar);
    }
    arena_free(&ar);
    arena_free(&keys);
}

static void BenchmarkVerification(TestingB *b) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    Ed25519PrivateKey priv;
    Ed25519PublicKey pub = ed25519_generate_key(a, zero_reader(), &priv, &err);
    if (BURROW_FAILED(err))
        testing_b_fatalf_v(b, "%s", error_text(err));
    Slice message = cs("Hello, world!");
    Slice signature = ed25519_sign(a, priv, message);
    testing_b_reset_timer(b);
    for (Int i = 0; i < testing_b_n(b); i++)
        (void)ed25519_verify(pub, message, signature);
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestGenerateKey)                                                                 \
    X(TestSignVerify)                                                                  \
    X(TestSignVerifyHashed)                                                            \
    X(TestSignVerifyContext)                                                           \
    X(TestCryptoSigner)                                                                \
    X(TestEqual)                                                                       \
    X(TestGolden)                                                                      \
    X(TestMalleability)                                                                \
    X(TestEd25519Vectors)                                                              \
    X(TestEd25519Wycheproof)                                                           \
    X(BenchmarkKeyGeneration)                                                          \
    X(BenchmarkNewKeyFromSeed)                                                         \
    X(BenchmarkSigning)                                                                \
    X(BenchmarkVerification)

TESTING_MAIN(TESTS)
