/* Derived from Go's src/crypto/internal/fips140test/fips_test.go and
 * fips140v1.26_test.go.
 * Go source: go1.27.1.
 *
 * Not in Go: Go calls the module's own packages. burrow has internal functions
 * for some of them, the CounterKDF, the CTR_DRBG, HMAC, HKDF's Extract and
 * Expand and the TLS key derivations, and those are called here. The rest go
 * through the public functions, which call the same code: AES and its modes,
 * GCM with a random nonce, ECDH, ECDSA, Ed25519, ML-KEM, ML-DSA, RSA, the
 * hashes, HKDF's Key and PBKDF2.
 *
 * Copyright 2024 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"
#include "fipstest.h"

#include "burrow/burrow.h"
#include "burrow/crypto.h"
#include "burrow/crypto/aes.h"
#include "burrow/crypto/cipher.h"
#include "burrow/crypto/ecdh.h"
#include "burrow/crypto/ecdsa.h"
#include "burrow/crypto/ed25519.h"
#include "burrow/crypto/elliptic.h"
#include "burrow/crypto/hkdf.h"
#include "burrow/crypto/mldsa.h"
#include "burrow/crypto/mlkem.h"
#include "burrow/crypto/pbkdf2.h"
#include "burrow/crypto/rand.h"
#include "burrow/crypto/rsa.h"
#include "burrow/crypto/sha256.h"
#include "burrow/crypto/sha3.h"
#include "burrow/crypto/sha512.h"
#include "burrow/encoding/hex.h"

#include "../src/crypto/aes_internal.h"
#include "../src/crypto/drbg_internal.h"
#include "../src/crypto/rsa_internal.h"
#include "../src/crypto/tls_internal.h"

#include <string.h>

/* Not in Go: burrow has no build info with a GOFIPS140 setting, so this is the
 * check Go makes when there is none. */
static void TestVersion(TestingT *t) {
    /* Without GOFIPS140, the Version should be "latest". */
    Str v = fips140_version();
    if (!str_eq(v, BURROW_S("latest")))
        testing_t_errorf_v(t, "Version is %q, expected latest", v);
}

static void fatal_if_err(TestingT *t, Error err) {
    testing_t_helper(t);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);
}

static Slice decode_hex(TestingT *t, Alloc *a, const char *s) {
    testing_t_helper(t);
    Error err = BURROW_NO_ERROR;
    Slice b = hex_decode_string(
        a, strings_replace_all(a, str_from_cstr(s), BURROW_S(" "), BURROW_S("")), &err);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);
    return b;
}

static void service_indicator_set(void *env) {
    TestingT *t = env;
    if (burrow__fips140_service_indicator())
        testing_t_logf_v(t, "Service indicator is set");
    else
        testing_t_errorf_v(t, "Service indicator is not set");
}

static void ensure_service_indicator(TestingT *t) {
    burrow__fips140_reset_service_indicator();
    testing_t_cleanup(t, BURROW_FN(Func, service_indicator_set, t));
}

static void service_indicator_not_set(void *env) {
    TestingT *t = env;
    if (!burrow__fips140_service_indicator())
        testing_t_logf_v(t, "Service indicator is not set");
    else
        testing_t_errorf_v(t, "Service indicator is set");
}

static void ensure_service_indicator_false(TestingT *t) {
    burrow__fips140_reset_service_indicator();
    testing_t_cleanup(t, BURROW_FN(Func, service_indicator_not_set, t));
}

/* Not in Go: the memory of a subtest, freed when it ends, even by a fatal
 * error. */
static void free_arena(void *env) {
    Arena *ar = env;
    arena_free(ar);
    mem_free(heap_allocator(), ar, sizeof *ar, _Alignof(Arena));
}

static Alloc *subtest_arena(TestingT *t) {
    Arena *ar = BURROW_NEW(heap_allocator(), Arena);
    if (ar == NULL)
        testing_t_fatal_v(t, "out of memory");
    arena_init(ar, NULL, 0);
    testing_t_cleanup(t, BURROW_FN(Func, free_arena, ar));
    return arena_allocator(ar);
}

/* Not in Go: what the closures share. */
typedef struct FipsEnv {
    Slice aes_key;
    Slice aes_iv;
    Slice plaintext;
    Slice plaintext_sha256;
    CipherBlock aes_block;
    RsaPrivateKey *rsa_key;
} FipsEnv;

static void hash_subtest(TestingT *t, Hash h, Slice plaintext, const char *name) {
    Error err = BURROW_NO_ERROR;
    hash_write(h, plaintext, &err);
    Slice out = hash_sum(subtest_arena(t), h, slice_nil(TYPE_BYTE));
    testing_t_logf_v(t, "%s output: %x", str_from_cstr(name), out);
}

static void shake_subtest(TestingT *t, Sha3SHAKE *h, Slice plaintext,
                          const char *name) {
    Error err = BURROW_NO_ERROR;
    sha3_shake_write(h, plaintext, &err);
    Byte buf[16];
    Slice out = slice_from(buf, 16, 16, TYPE_BYTE);
    sha3_shake_read(h, out, &err);
    testing_t_logf_v(t, "%s output: %x", str_from_cstr(name), out);
}

static void ml_dsa(void *env, TestingT *t) {
    const FipsEnv *e = env;
    ensure_service_indicator(t);
    Alloc *a = subtest_arena(t);
    Error err = BURROW_NO_ERROR;
    MldsaPrivateKey *k = mldsa_generate_key(a, mldsa_mldsa44(), &err);
    fatal_if_err(t, err);

    Slice sig = mldsa_private_key_sign_deterministic(k, a, e->plaintext,
                                                     (CryptoSignerOpts){0}, &err);
    fatal_if_err(t, err);
    testing_t_logf_v(t, "ML-DSA signature: %x", sig);

    err = mldsa_verify(mldsa_private_key_public_key(k), e->plaintext, sig, NULL);
    fatal_if_err(t, err);
}

static void test_fips140v126(TestingT *t, const FipsEnv *e) {
    testing_t_run(t, BURROW_S("ML-DSA KeyGen, SigGen, SigVer"),
                  BURROW_FN(TestingTFunc, ml_dsa, (void *)(uintptr_t)e));
}

static void aes_ctr(void *env, TestingT *t) {
    const FipsEnv *e = env;
    ensure_service_indicator(t);
    Alloc *a = subtest_arena(t);
    CipherStream ctr = cipher_new_ctr(a, e->aes_block, e->aes_iv);
    Slice ciphertext = slice_make(a, TYPE_BYTE, e->plaintext.len, e->plaintext.len);
    cipher_stream_xor_key_stream(ctr, ciphertext, e->plaintext);
    testing_t_logf_v(t, "AES-CTR ciphertext: %x", ciphertext);
    Slice out = slice_make(a, TYPE_BYTE, e->plaintext.len, e->plaintext.len);
    ctr = cipher_new_ctr(a, e->aes_block, e->aes_iv);
    cipher_stream_xor_key_stream(ctr, out, ciphertext);
    testing_t_logf_v(t, "AES-CTR decrypted plaintext: %s", out);
    if (!bytes_equal(e->plaintext, out))
        testing_t_errorf_v(t, "AES-CTR round trip failed");
}

static void aes_cbc(void *env, TestingT *t) {
    const FipsEnv *e = env;
    ensure_service_indicator(t);
    Alloc *a = subtest_arena(t);
    CipherBlockMode cbc_enc = cipher_new_cbc_encrypter(a, e->aes_block, e->aes_iv);
    Slice ciphertext = slice_make(a, TYPE_BYTE, e->plaintext.len, e->plaintext.len);
    cipher_block_mode_crypt_blocks(cbc_enc, ciphertext, e->plaintext);
    testing_t_logf_v(t, "AES-CBC ciphertext: %x", ciphertext);
    CipherBlockMode cbc_dec = cipher_new_cbc_decrypter(a, e->aes_block, e->aes_iv);
    Slice out = slice_make(a, TYPE_BYTE, e->plaintext.len, e->plaintext.len);
    cipher_block_mode_crypt_blocks(cbc_dec, out, ciphertext);
    testing_t_logf_v(t, "AES-CBC decrypted plaintext: %s", out);
    if (!bytes_equal(e->plaintext, out))
        testing_t_errorf_v(t, "AES-CBC round trip failed");
}

/* Not in Go: Go seals with gcm.SealWithRandomNonce over gcm.New, which puts
 * the nonce in a slice of its own. cipher_new_gcm_with_random_nonce calls the
 * same, and puts the nonce in front of the ciphertext. */
static void aes_gcm(void *env, TestingT *t) {
    const FipsEnv *e = env;
    ensure_service_indicator(t);
    Alloc *a = subtest_arena(t);
    Error err = BURROW_NO_ERROR;
    CipherAEAD g = cipher_new_gcm_with_random_nonce(a, e->aes_block, &err);
    fatal_if_err(t, err);
    Slice ciphertext =
        cipher_aead_seal(g, a, slice_nil(TYPE_BYTE), slice_nil(TYPE_BYTE), e->plaintext,
                         slice_nil(TYPE_BYTE));
    testing_t_logf_v(t, "AES-GCM ciphertext: %x", ciphertext);
    Slice out = cipher_aead_open(g, a, slice_nil(TYPE_BYTE), slice_nil(TYPE_BYTE),
                                 ciphertext, slice_nil(TYPE_BYTE), &err);
    fatal_if_err(t, err);
    testing_t_logf_v(t, "AES-GCM decrypted plaintext: %s", out);
    if (!bytes_equal(e->plaintext, out))
        testing_t_errorf_v(t, "AES-GCM round trip failed");
}

static void counter_kdf(void *env, TestingT *t) {
    const FipsEnv *e = env;
    ensure_service_indicator(t);
    CounterKdf k;
    burrow__counter_kdf_init(&k, burrow__aes_block_of(e->aes_block));
    const Byte context[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    Byte key[32];
    burrow__counter_kdf_derive_key(&k, key, 0x01, context);
    testing_t_logf_v(t, "Counter KDF key: %x", slice_from(key, 32, 32, TYPE_BYTE));
}

static void kas_ecc_ssc(void *env, TestingT *t) {
    (void)env;
    ensure_service_indicator(t);
    Alloc *a = subtest_arena(t);
    Error err = BURROW_NO_ERROR;
    EcdhPrivateKey *k =
        ecdh_curve_generate_key(ecdh_p256(), a, crypto_rand_reader, &err);
    fatal_if_err(t, err);
    const EcdhPublicKey *pk = ecdh_private_key_public_key(k);
    Slice shared = ecdh_private_key_ecdh(k, a, pk, &err);
    fatal_if_err(t, err);
    testing_t_logf_v(t, "KAS-ECC-SSC shared secret: %x", shared);
}

static void ecdsa_sig(void *env, TestingT *t) {
    const FipsEnv *e = env;
    ensure_service_indicator(t);
    Alloc *a = subtest_arena(t);
    Error err = BURROW_NO_ERROR;
    EcdsaPrivateKey *k =
        ecdsa_generate_key(a, elliptic_p256(), crypto_rand_reader, &err);
    fatal_if_err(t, err);

    Slice sig = ecdsa_sign_asn1(a, crypto_rand_reader, k, e->plaintext_sha256, &err);
    fatal_if_err(t, err);
    testing_t_logf_v(t, "ECDSA signature: %x", sig);
    if (!ecdsa_verify_asn1(&k->public_key, e->plaintext_sha256, sig))
        testing_t_errorf_v(t, "ECDSA signature verification failed");

    CryptoHash h = CRYPTO_SHA256;
    sig = ecdsa_private_key_sign(k, a, (IoReader){0}, e->plaintext_sha256,
                                 crypto_hash_as_signer_opts(&h), &err);
    fatal_if_err(t, err);
    testing_t_logf_v(t, "ECDSA deterministic signature: %x", sig);
    if (!ecdsa_verify_asn1(&k->public_key, e->plaintext_sha256, sig))
        testing_t_errorf_v(t, "ECDSA deterministic signature verification failed");
}

static void eddsa(void *env, TestingT *t) {
    const FipsEnv *e = env;
    ensure_service_indicator(t);
    Alloc *a = subtest_arena(t);
    Error err = BURROW_NO_ERROR;
    Ed25519PrivateKey k = {0};
    Ed25519PublicKey pk = ed25519_generate_key(a, crypto_rand_reader, &k, &err);
    fatal_if_err(t, err);

    Slice sig = ed25519_sign(a, k, e->plaintext);
    testing_t_logf_v(t, "EDDSA signature: %x", sig);

    if (!ed25519_verify(pk, e->plaintext, sig))
        testing_t_errorf_v(t, "EDDSA signature verification failed");
}

static void ctr_drbg(void *env, TestingT *t) {
    const FipsEnv *e = env;
    ensure_service_indicator(t);
    const Byte *p = e->plaintext.p;
    DrbgCounter r;
    burrow__drbg_new_counter(&r, p);
    burrow__drbg_counter_reseed(&r, p, p);
    Byte out[16];
    burrow__drbg_counter_generate(&r, slice_from(out, 16, 16, TYPE_BYTE), p);
    testing_t_logf_v(t, "ctrDRBG output: %x", slice_from(out, 16, 16, TYPE_BYTE));
}

static void hmac(void *env, TestingT *t) {
    const FipsEnv *e = env;
    ensure_service_indicator(t);
    Alloc *a = subtest_arena(t);
    Error err = BURROW_NO_ERROR;
    Hash h = burrow__hmac_new(a, sha256_new, e->plaintext);
    if (h.vt == NULL)
        testing_t_fatal_v(t, "out of memory");
    hash_write(h, e->plaintext, &err);
    Slice out = hash_sum(a, h, slice_nil(TYPE_BYTE));
    testing_t_logf_v(t, "HMAC output: %x", out);
}

static void ml_kem(void *env, TestingT *t) {
    (void)env;
    ensure_service_indicator(t);
    Alloc *a = subtest_arena(t);
    Error err = BURROW_NO_ERROR;
    MlkemDecapsulationKey768 *k = mlkem_generate_key768(a, &err);
    fatal_if_err(t, err);

    Slice c = slice_nil(TYPE_BYTE);
    Slice ss = mlkem_encapsulation_key768_encapsulate(
        mlkem_decapsulation_key768_encapsulation_key(k), a, &c);
    testing_t_logf_v(t, "ML-KEM encapsulation: %x", c);

    Slice ss2 = mlkem_decapsulation_key768_decapsulate(k, a, c, &err);
    fatal_if_err(t, err);
    testing_t_logf_v(t, "ML-KEM shared secret: %x", ss);
    if (!bytes_equal(ss, ss2))
        testing_t_errorf_v(t, "ML-KEM round trip failed");
}

/* Not in Go: the key lives as long as the test, so it comes from the heap and
 * the test frees it. */
static void rsa_key_gen(void *env, TestingT *t) {
    FipsEnv *e = env;
    ensure_service_indicator(t);
    Error err = BURROW_NO_ERROR;
    e->rsa_key = rsa_generate_key(heap_allocator(), crypto_rand_reader, 2048, &err);
    fatal_if_err(t, err);
    testing_t_logf_v(t, "RSA key generated");
}

/* Not in Go: a check that RSA KeyGen made the key, since the subtests that
 * use it would crash on a NULL one. */
static void need_rsa_key(TestingT *t, const FipsEnv *e) {
    if (e->rsa_key == NULL)
        testing_t_fatal_v(t, "no RSA key");
}

static void rsa_pkcs1v15(void *env, TestingT *t) {
    const FipsEnv *e = env;
    ensure_service_indicator(t);
    need_rsa_key(t, e);
    Alloc *a = subtest_arena(t);
    Error err = BURROW_NO_ERROR;
    Slice sig = rsa_sign_pkcs1_v15(a, (IoReader){0}, e->rsa_key, CRYPTO_SHA256,
                                   e->plaintext_sha256, &err);
    fatal_if_err(t, err);
    testing_t_logf_v(t, "RSA PKCS1v15 signature: %x", sig);

    err = rsa_verify_pkcs1_v15(&e->rsa_key->public_key, CRYPTO_SHA256,
                               e->plaintext_sha256, sig);
    fatal_if_err(t, err);
}

static void rsa_pss(void *env, TestingT *t) {
    const FipsEnv *e = env;
    ensure_service_indicator(t);
    need_rsa_key(t, e);
    Alloc *a = subtest_arena(t);
    Error err = BURROW_NO_ERROR;
    RsaPSSOptions opts = {16, 0};
    Slice sig = rsa_sign_pss(a, crypto_rand_reader, e->rsa_key, CRYPTO_SHA256,
                             e->plaintext_sha256, &opts, &err);
    fatal_if_err(t, err);
    testing_t_logf_v(t, "RSA PSS signature: %x", sig);

    err = rsa_verify_pss(&e->rsa_key->public_key, CRYPTO_SHA256, e->plaintext_sha256,
                         sig, NULL);
    fatal_if_err(t, err);
}

static void rsa_godebug_reset(void *env) {
    (void)env;
    burrow__rsa_godebug_set(NULL);
}

/* Not in Go: rsa_generate_key refuses keys under 1024 bits unless GODEBUG has
 * rsa1024min=0, where Go's module function does not check, so the subtest
 * sets it. */
static void rsa_small_key(void *env, TestingT *t) {
    (void)env;
    ensure_service_indicator_false(t);
    Alloc *a = subtest_arena(t);
    burrow__rsa_godebug_set("rsa1024min=0");
    testing_t_cleanup(t, BURROW_FN(Func, rsa_godebug_reset, NULL));
    Error err = BURROW_NO_ERROR;
    (void)rsa_generate_key(a, crypto_rand_reader, 512, &err);
    fatal_if_err(t, err);
    testing_t_logf_v(t, "RSA key generated");
}

static void kts_ifc_oaep(void *env, TestingT *t) {
    const FipsEnv *e = env;
    ensure_service_indicator(t);
    need_rsa_key(t, e);
    Alloc *a = subtest_arena(t);
    Error err = BURROW_NO_ERROR;
    Slice c =
        rsa_encrypt_oaep(a, sha256_new(a), crypto_rand_reader, &e->rsa_key->public_key,
                         e->plaintext_sha256, slice_nil(TYPE_BYTE), &err);
    fatal_if_err(t, err);
    testing_t_logf_v(t, "RSA OAEP ciphertext: %x", c);

    Slice out = rsa_decrypt_oaep(a, sha256_new(a), (IoReader){0}, e->rsa_key, c,
                                 slice_nil(TYPE_BYTE), &err);
    fatal_if_err(t, err);
    testing_t_logf_v(t, "RSA OAEP decrypted plaintext: %x", out);
    if (!bytes_equal(e->plaintext_sha256, out))
        testing_t_errorf_v(t, "RSA OAEP round trip failed");
}

static void sha2_224(void *env, TestingT *t) {
    const FipsEnv *e = env;
    ensure_service_indicator(t);
    hash_subtest(t, sha256_new224(subtest_arena(t)), e->plaintext, "SHA2-224");
}

static void sha2_256(void *env, TestingT *t) {
    const FipsEnv *e = env;
    ensure_service_indicator(t);
    hash_subtest(t, sha256_new(subtest_arena(t)), e->plaintext, "SHA2-256");
}

static void sha2_384(void *env, TestingT *t) {
    const FipsEnv *e = env;
    ensure_service_indicator(t);
    hash_subtest(t, sha512_new384(subtest_arena(t)), e->plaintext, "SHA2-384");
}

static void sha2_512(void *env, TestingT *t) {
    const FipsEnv *e = env;
    ensure_service_indicator(t);
    hash_subtest(t, sha512_new(subtest_arena(t)), e->plaintext, "SHA2-512");
}

static void sha2_512_224(void *env, TestingT *t) {
    const FipsEnv *e = env;
    ensure_service_indicator(t);
    hash_subtest(t, sha512_new512224(subtest_arena(t)), e->plaintext, "SHA2-512/224");
}

static void sha2_512_256(void *env, TestingT *t) {
    const FipsEnv *e = env;
    ensure_service_indicator(t);
    hash_subtest(t, sha512_new512256(subtest_arena(t)), e->plaintext, "SHA2-512/256");
}

static void sha3_224(void *env, TestingT *t) {
    const FipsEnv *e = env;
    ensure_service_indicator(t);
    hash_subtest(t, sha3_as_hash(sha3_new224(subtest_arena(t))), e->plaintext,
                 "SHA3-224");
}

static void sha3_256(void *env, TestingT *t) {
    const FipsEnv *e = env;
    ensure_service_indicator(t);
    hash_subtest(t, sha3_as_hash(sha3_new256(subtest_arena(t))), e->plaintext,
                 "SHA3-256");
}

static void sha3_384(void *env, TestingT *t) {
    const FipsEnv *e = env;
    ensure_service_indicator(t);
    hash_subtest(t, sha3_as_hash(sha3_new384(subtest_arena(t))), e->plaintext,
                 "SHA3-384");
}

static void sha3_512(void *env, TestingT *t) {
    const FipsEnv *e = env;
    ensure_service_indicator(t);
    hash_subtest(t, sha3_as_hash(sha3_new512(subtest_arena(t))), e->plaintext,
                 "SHA3-512");
}

static void shake_128(void *env, TestingT *t) {
    const FipsEnv *e = env;
    ensure_service_indicator(t);
    shake_subtest(t, sha3_new_shake128(subtest_arena(t)), e->plaintext, "SHAKE-128");
}

static void shake_256(void *env, TestingT *t) {
    const FipsEnv *e = env;
    ensure_service_indicator(t);
    shake_subtest(t, sha3_new_shake256(subtest_arena(t)), e->plaintext, "SHAKE-256");
}

static void cshake_128(void *env, TestingT *t) {
    const FipsEnv *e = env;
    ensure_service_indicator(t);
    Alloc *a = subtest_arena(t);
    shake_subtest(t,
                  sha3_new_cshake128(a, slice_nil(TYPE_BYTE),
                                     slice_from_str(a, BURROW_S("test"))),
                  e->plaintext, "cSHAKE-128");
}

static void cshake_256(void *env, TestingT *t) {
    const FipsEnv *e = env;
    ensure_service_indicator(t);
    Alloc *a = subtest_arena(t);
    shake_subtest(t,
                  sha3_new_cshake256(a, slice_nil(TYPE_BYTE),
                                     slice_from_str(a, BURROW_S("test"))),
                  e->plaintext, "cSHAKE-256");
}

static void kda_hkdf(void *env, TestingT *t) {
    const FipsEnv *e = env;
    ensure_service_indicator(t);
    Alloc *a = subtest_arena(t);
    Error err = BURROW_NO_ERROR;
    Slice key =
        hkdf_key(a, sha256_new, e->plaintext_sha256,
                 slice_from_str(a, BURROW_S("salt")), BURROW_S("info"), 16, &err);
    testing_t_logf_v(t, "HKDF key: %x", key);
}

static void kda_one_step_no_counter(void *env, TestingT *t) {
    const FipsEnv *e = env;
    ensure_service_indicator(t);
    Alloc *a = subtest_arena(t);
    Error err = BURROW_NO_ERROR;
    Slice key = burrow__hkdf_extract(a, sha256_new, e->plaintext_sha256,
                                     slice_from_str(a, BURROW_S("salt")), &err);
    testing_t_logf_v(t, "KDA OneStepNoCounter key: %x", key);
}

static void feedback_kdf(void *env, TestingT *t) {
    const FipsEnv *e = env;
    ensure_service_indicator(t);
    Alloc *a = subtest_arena(t);
    Error err = BURROW_NO_ERROR;
    Slice key = burrow__hkdf_expand(a, sha256_new, e->plaintext_sha256,
                                    BURROW_S("info"), 16, &err);
    testing_t_logf_v(t, "Feedback KDF key: %x", key);
}

static void pbkdf(void *env, TestingT *t) {
    const FipsEnv *e = env;
    ensure_service_indicator(t);
    Alloc *a = subtest_arena(t);
    Error err = BURROW_NO_ERROR;
    Slice key = pbkdf2_key(a, sha256_new, BURROW_S("password"), e->plaintext_sha256, 2,
                           16, &err);
    fatal_if_err(t, err);
    testing_t_logf_v(t, "PBKDF key: %x", key);
}

static void kdf_tls12(void *env, TestingT *t) {
    const FipsEnv *e = env;
    ensure_service_indicator(t);
    Alloc *a = subtest_arena(t);
    Slice key = burrow__tls12_master_secret(a, sha256_new, e->plaintext_sha256,
                                            slice_from_str(a, BURROW_S("test")));
    testing_t_logf_v(t, "TLS v1.2 CVL Master Secret: %x", key);
}

static void kdf_tls13(void *env, TestingT *t) {
    const FipsEnv *e = env;
    ensure_service_indicator(t);
    Alloc *a = subtest_arena(t);
    Tls13EarlySecret es =
        burrow__tls13_new_early_secret(a, sha256_new, e->plaintext_sha256);
    Tls13HandshakeSecret hs =
        burrow__tls13_early_secret_handshake_secret(&es, a, e->plaintext_sha256);
    Tls13MasterSecret ms = burrow__tls13_handshake_secret_master_secret(&hs, a);
    Slice client = burrow__tls13_master_secret_client_application_traffic_secret(
        &ms, a, sha256_new(a));
    Slice server = burrow__tls13_master_secret_server_application_traffic_secret(
        &ms, a, sha256_new(a));
    testing_t_logf_v(t,
                     "TLS v1.3 CVL Application Traffic Secrets: client %x, server %x",
                     client, server);
}

static void TestFIPS140(TestingT *t) {
    module_status(t);

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    FipsEnv e = {0};
    e.aes_key = slice_make(a, TYPE_BYTE, 128 / 8, 128 / 8);
    e.aes_iv = slice_make(a, TYPE_BYTE, AES_BLOCK_SIZE, AES_BLOCK_SIZE);
    e.plaintext =
        slice_from_str(a, BURROW_S("Go Cryptographic Module TestFIPS140 plaintext..."));
    e.plaintext_sha256 = decode_hex(
        t, a, "06b2614e2ef315832b23f5d0ff70294d8ddd3889527dfbe75707fe41da929325");
    Error err = BURROW_NO_ERROR;
    e.aes_block = aes_new_cipher(a, e.aes_key, &err);
    fatal_if_err(t, err);

    test_fips140v126(t, &e);

    testing_t_run(t, BURROW_S("AES-CTR"), BURROW_FN(TestingTFunc, aes_ctr, &e));
    testing_t_run(t, BURROW_S("AES-CBC"), BURROW_FN(TestingTFunc, aes_cbc, &e));
    testing_t_run(t, BURROW_S("AES-GCM"), BURROW_FN(TestingTFunc, aes_gcm, &e));
    testing_t_run(t, BURROW_S("Counter KDF"), BURROW_FN(TestingTFunc, counter_kdf, &e));
    testing_t_run(t, BURROW_S("KAS-ECC-SSC ephemeralUnified"),
                  BURROW_FN(TestingTFunc, kas_ecc_ssc, &e));
    testing_t_run(t, BURROW_S("ECDSA KeyGen, SigGen, SigVer"),
                  BURROW_FN(TestingTFunc, ecdsa_sig, &e));
    testing_t_run(t, BURROW_S("EDDSA KeyGen, SigGen, SigVer"),
                  BURROW_FN(TestingTFunc, eddsa, &e));
    testing_t_run(t, BURROW_S("ctrDRBG"), BURROW_FN(TestingTFunc, ctr_drbg, &e));
    testing_t_run(t, BURROW_S("HMAC"), BURROW_FN(TestingTFunc, hmac, &e));
    testing_t_run(t, BURROW_S("ML-KEM KeyGen, Encap, Decap"),
                  BURROW_FN(TestingTFunc, ml_kem, &e));
    testing_t_run(t, BURROW_S("RSA KeyGen"), BURROW_FN(TestingTFunc, rsa_key_gen, &e));
    testing_t_run(t, BURROW_S("RSA SigGen, SigVer PKCS 1.5"),
                  BURROW_FN(TestingTFunc, rsa_pkcs1v15, &e));
    testing_t_run(t, BURROW_S("RSA SigGen, SigVer PSS"),
                  BURROW_FN(TestingTFunc, rsa_pss, &e));
    testing_t_run(t, BURROW_S("RSA KeyGen w/ small key [NOT APPROVED]"),
                  BURROW_FN(TestingTFunc, rsa_small_key, &e));
    testing_t_run(t, BURROW_S("KTS IFC OAEP"),
                  BURROW_FN(TestingTFunc, kts_ifc_oaep, &e));
    testing_t_run(t, BURROW_S("SHA2-224"), BURROW_FN(TestingTFunc, sha2_224, &e));
    testing_t_run(t, BURROW_S("SHA2-256"), BURROW_FN(TestingTFunc, sha2_256, &e));
    testing_t_run(t, BURROW_S("SHA2-384"), BURROW_FN(TestingTFunc, sha2_384, &e));
    testing_t_run(t, BURROW_S("SHA2-512"), BURROW_FN(TestingTFunc, sha2_512, &e));
    testing_t_run(t, BURROW_S("SHA2-512/224"),
                  BURROW_FN(TestingTFunc, sha2_512_224, &e));
    testing_t_run(t, BURROW_S("SHA2-512/256"),
                  BURROW_FN(TestingTFunc, sha2_512_256, &e));
    testing_t_run(t, BURROW_S("SHA3-224"), BURROW_FN(TestingTFunc, sha3_224, &e));
    testing_t_run(t, BURROW_S("SHA3-256"), BURROW_FN(TestingTFunc, sha3_256, &e));
    testing_t_run(t, BURROW_S("SHA3-384"), BURROW_FN(TestingTFunc, sha3_384, &e));
    testing_t_run(t, BURROW_S("SHA3-512"), BURROW_FN(TestingTFunc, sha3_512, &e));
    testing_t_run(t, BURROW_S("SHAKE-128"), BURROW_FN(TestingTFunc, shake_128, &e));
    testing_t_run(t, BURROW_S("SHAKE-256"), BURROW_FN(TestingTFunc, shake_256, &e));
    testing_t_run(t, BURROW_S("cSHAKE-128"), BURROW_FN(TestingTFunc, cshake_128, &e));
    testing_t_run(t, BURROW_S("cSHAKE-256"), BURROW_FN(TestingTFunc, cshake_256, &e));
    testing_t_run(t, BURROW_S("KDA HKDF"), BURROW_FN(TestingTFunc, kda_hkdf, &e));
    testing_t_run(t, BURROW_S("KDA OneStepNoCounter"),
                  BURROW_FN(TestingTFunc, kda_one_step_no_counter, &e));
    testing_t_run(t, BURROW_S("Feedback KDF"),
                  BURROW_FN(TestingTFunc, feedback_kdf, &e));
    testing_t_run(t, BURROW_S("PBKDF"), BURROW_FN(TestingTFunc, pbkdf, &e));
    testing_t_run(t, BURROW_S("KDF TLS v1.2 CVL"),
                  BURROW_FN(TestingTFunc, kdf_tls12, &e));
    testing_t_run(t, BURROW_S("KDF TLS v1.3 CVL"),
                  BURROW_FN(TestingTFunc, kdf_tls13, &e));

    if (e.rsa_key != NULL)
        rsa_private_key_free(e.rsa_key, heap_allocator());
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestVersion)                                                                     \
    X(TestFIPS140)

TESTING_MAIN(TESTS)
