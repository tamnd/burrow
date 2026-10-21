/* Derived from Go's src/crypto/internal/fips140only/fips140only_test.go.
 * Go source: go1.27.1.
 *
 * Not in Go: Go passes closures to expectPanic and to withApprovedHash and
 * withNonApprovedHash. Here the calls that should panic are small functions
 * that expect_panic runs, the two lists of hashes are tables, and the loops
 * over them are written out. The errRet2 and errRet3 of a call are ERR_RET of
 * it, with &ret_err for its error. MustMinimumFIPS140ModuleVersion is left
 * out, since burrow's module version is always "latest", which it lets
 * through.
 *
 * Copyright 2025 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"
#include "fipstest.h"

#include "burrow/burrow.h"
#include "burrow/crypto.h"
#include "burrow/crypto/aes.h"
#include "burrow/crypto/cipher.h"
#include "burrow/crypto/des.h"
#include "burrow/crypto/dsa.h"
#include "burrow/crypto/ecdh.h"
#include "burrow/crypto/ecdsa.h"
#include "burrow/crypto/ed25519.h"
#include "burrow/crypto/elliptic.h"
#include "burrow/crypto/fips140.h"
#include "burrow/crypto/hkdf.h"
#include "burrow/crypto/hmac.h"
#include "burrow/crypto/hpke.h"
#include "burrow/crypto/md5.h"
#include "burrow/crypto/mlkem.h"
#include "burrow/crypto/mlkem/mlkemtest.h"
#include "burrow/crypto/pbkdf2.h"
#include "burrow/crypto/rand.h"
#include "burrow/crypto/rc4.h"
#include "burrow/crypto/rsa.h"
#include "burrow/crypto/sha1.h"
#include "burrow/crypto/sha256.h"
#include "burrow/crypto/x509.h"
#include "burrow/encoding/pem.h"

#include "../src/crypto/chacha20poly1305.h"
#include "../src/crypto/fips140_module.h"
#include "../src/crypto/rand_internal.h"

#include <string.h>

#define S(x) BURROW_S(x)

#define ERR_RET(call) (ret_err = BURROW_NO_ERROR, (void)(call), ret_err)

static Slice zeros(Alloc *a, Int n) {
    return slice_make(a, TYPE_BYTE, n, n);
}

/* Not in Go: the value cryptocustomrand has in GODEBUG, which the subtest
 * sets. */
static const char *cryptocustomrand = "";

typedef struct SavedGODEBUG {
    Arena ar;
    bool found;
    Str old;
} SavedGODEBUG;

static void restore_godebug(void *env) {
    SavedGODEBUG *s = env;
    if (s->found)
        (void)os_setenv(S("GODEBUG"), s->old);
    else
        (void)os_unsetenv(S("GODEBUG"));
    burrow__crypto_rand_godebug_set(NULL);
    arena_free(&s->ar);
    mem_free(heap_allocator(), s, sizeof *s, _Alignof(SavedGODEBUG));
}

/* t.Setenv("GODEBUG", os.Getenv("GODEBUG")+extra). Not in Go: crypto/rand
 * reads GODEBUG once, so it is given the new value as well. */
static void setenv_godebug(TestingT *t, const char *extra) {
    SavedGODEBUG *s = BURROW_NEW(heap_allocator(), SavedGODEBUG);
    if (s == NULL)
        testing_t_fatal_v(t, "out of memory");
    arena_init(&s->ar, NULL, 0);
    Alloc *a = arena_allocator(&s->ar);
    s->old = os_lookup_env(a, S("GODEBUG"), &s->found);
    testing_t_cleanup(t, BURROW_FN(Func, restore_godebug, s));
    Str v = fmt_sprintf_v(a, "%s%s", s->old, str_from_cstr(extra));
    if (BURROW_FAILED(os_setenv(S("GODEBUG"), v)))
        testing_t_fatalf_v(t, "Setenv(GODEBUG, %q) failed", v);
    burrow__crypto_rand_godebug_set(str_to_cstr(a, v));
}

/* blockWrap: a cipher.Block that is not the AES one, though it does AES. */
static Int wrap_block_size(void *self) {
    const CipherBlock *b = self;
    return b->vt->block_size(b->data);
}

static void wrap_encrypt(void *self, Slice dst, Slice src) {
    const CipherBlock *b = self;
    b->vt->encrypt(b->data, dst, src);
}

static void wrap_decrypt(void *self, Slice dst, Slice src) {
    const CipherBlock *b = self;
    b->vt->decrypt(b->data, dst, src);
}

static const CipherBlockVT block_wrap_vt = {NULL, wrap_block_size, wrap_encrypt,
                                            wrap_decrypt};

/* readerWrap: an io.Reader that is not crypto/rand.Reader, though it reads
 * from it. */
static Int wrap_read(void *self, Slice p, Error *err) {
    (void)self;
    return crypto_rand_reader.vt->read(crypto_rand_reader.data, p, err);
}

static const IoReaderVT reader_wrap_vt = {NULL, wrap_read};

static const IoReader reader_wrap = {&reader_wrap_vt, NULL};

static const IoReader nil_reader = {NULL, NULL};

/* What the functions that should panic are called with. */
typedef struct Only {
    Alloc *a;
    CipherBlock aes_block;
    CipherBlock not_aes_block;
    Slice iv;
    CryptoHash h;
    HashNewFunc h_new;
} Only;

static void new_cbc_encrypter_not_aes(Only *o) {
    (void)cipher_new_cbc_encrypter(o->a, o->not_aes_block, o->iv);
}

static void new_cbc_decrypter_not_aes(Only *o) {
    (void)cipher_new_cbc_decrypter(o->a, o->not_aes_block, o->iv);
}

static void new_cfb_encrypter(Only *o) {
    (void)cipher_new_cfb_encrypter(o->a, o->aes_block, o->iv);
}

static void new_cfb_decrypter(Only *o) {
    (void)cipher_new_cfb_decrypter(o->a, o->aes_block, o->iv);
}

static void new_ctr_not_aes(Only *o) {
    (void)cipher_new_ctr(o->a, o->not_aes_block, o->iv);
}

static void new_ofb(Only *o) {
    (void)cipher_new_ofb(o->a, o->aes_block, o->iv);
}

static void md5_new_sum(Only *o) {
    (void)hash_sum(o->a, md5_new(o->a), slice_nil(TYPE_BYTE));
}

static void md5_sum_foo(Only *o) {
    (void)o;
    (void)md5_sum(slice_from((void *)(uintptr_t)"foo", 3, 3, TYPE_BYTE));
}

static void sha1_new_sum(Only *o) {
    (void)hash_sum(o->a, sha1_new(o->a), slice_nil(TYPE_BYTE));
}

static void sha1_sum_foo(Only *o) {
    (void)o;
    (void)sha1_sum(slice_from((void *)(uintptr_t)"foo", 3, 3, TYPE_BYTE));
}

static void hash_new_sum(Only *o) {
    (void)hash_sum(o->a, crypto_hash_new(o->h, o->a), slice_nil(TYPE_BYTE));
}

static void hmac_new_short_key(Only *o) {
    (void)hmac_new(o->a, sha256_new, zeros(o->a, 10));
}

static void hmac_new_hash(Only *o) {
    (void)hmac_new(o->a, o->h_new, zeros(o->a, 16));
}

static void dsa_verify_empty(Only *o) {
    DsaPublicKey pub;
    memset(&pub, 0, sizeof pub);
    (void)dsa_verify(&pub, zeros(o->a, 16), big_new_int(o->a, 1), big_new_int(o->a, 1));
}

static Hash new_md5(Alloc *a) {
    return crypto_hash_new(CRYPTO_MD5, a);
}
static Hash new_sha1(Alloc *a) {
    return crypto_hash_new(CRYPTO_SHA1, a);
}
static Hash new_sha224(Alloc *a) {
    return crypto_hash_new(CRYPTO_SHA224, a);
}
static Hash new_sha256(Alloc *a) {
    return crypto_hash_new(CRYPTO_SHA256, a);
}
static Hash new_sha384(Alloc *a) {
    return crypto_hash_new(CRYPTO_SHA384, a);
}
static Hash new_sha512(Alloc *a) {
    return crypto_hash_new(CRYPTO_SHA512, a);
}
static Hash new_sha3_224(Alloc *a) {
    return crypto_hash_new(CRYPTO_SHA3_224, a);
}
static Hash new_sha3_256(Alloc *a) {
    return crypto_hash_new(CRYPTO_SHA3_256, a);
}
static Hash new_sha3_384(Alloc *a) {
    return crypto_hash_new(CRYPTO_SHA3_384, a);
}
static Hash new_sha3_512(Alloc *a) {
    return crypto_hash_new(CRYPTO_SHA3_512, a);
}
static Hash new_sha512_224(Alloc *a) {
    return crypto_hash_new(CRYPTO_SHA512_224, a);
}
static Hash new_sha512_256(Alloc *a) {
    return crypto_hash_new(CRYPTO_SHA512_256, a);
}

typedef struct HashCase {
    CryptoHash h;
    HashNewFunc new_hash;
} HashCase;

/* withApprovedHash. */
static const HashCase approved_hashes[] = {
    {CRYPTO_SHA224, new_sha224},         {CRYPTO_SHA256, new_sha256},
    {CRYPTO_SHA384, new_sha384},         {CRYPTO_SHA512, new_sha512},
    {CRYPTO_SHA3_224, new_sha3_224},     {CRYPTO_SHA3_256, new_sha3_256},
    {CRYPTO_SHA3_384, new_sha3_384},     {CRYPTO_SHA3_512, new_sha3_512},
    {CRYPTO_SHA512_224, new_sha512_224}, {CRYPTO_SHA512_256, new_sha512_256},
};

/* withNonApprovedHash. */
static const HashCase non_approved_hashes[] = {
    {CRYPTO_MD5, new_md5},
    {CRYPTO_SHA1, new_sha1},
};

#define N_APPROVED ((int)(sizeof approved_hashes / sizeof approved_hashes[0]))
#define N_NON_APPROVED                                                                 \
    ((int)(sizeof non_approved_hashes / sizeof non_approved_hashes[0]))

/* Not in Go: the deferred function, apart from the function that recovers, so
 * that nothing it makes lives across the setjmp. n is -1 when there was no
 * panic. */
static void expect_panic_recovered(TestingT *t, bool is_string, const char *msg,
                                   Int n) {
    testing_t_helper(t);
    if (n < 0) {
        testing_t_errorf_v(t, "expected panic");
        return;
    }
    Str s = str_from_bytes((const Byte *)msg, n);
    if (!is_string || !strings_contains(s, S("FIPS 140-only")))
        testing_t_errorf_v(t, "unexpected panic: %s", s);
}

static void expect_panic(TestingT *t, void (*f)(Only *), Only *o) {
    testing_t_helper(t);
    char msg[256];
    volatile Int n = -1;
    volatile bool is_string = false;
    BURROW_TRY {
        f(o);
    }
    BURROW_CATCH(r) {
        is_string = r.t == TYPE_STRING;
        Str s = panic_text(r);
        n = s.len < (Int)sizeof msg ? s.len : (Int)sizeof msg;
        memcpy(msg, s.p, (size_t)n);
    }
    BURROW_TRY_END;
    expect_panic_recovered(t, is_string, msg, n);
}

static void expect_err(TestingT *t, Error err) {
    testing_t_helper(t);
    if (!BURROW_FAILED(err))
        testing_t_errorf_v(t, "expected error");
    else if (!strings_contains(error_text(err), S("FIPS 140-only")))
        testing_t_errorf_v(t, "unexpected error: %s", error_text(err));
}

static void expect_no_err(TestingT *t, Error err) {
    testing_t_helper(t);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "unexpected error: %s", error_text(err));
}

static void expect_err_if_custom_rand(TestingT *t, Error err) {
    testing_t_helper(t);
    if (strcmp(cryptocustomrand, "1") == 0)
        expect_err(t, err);
    else
        expect_no_err(t, err);
}

static Str testing_key(Alloc *a, Str s) {
    return strings_replace_all(a, s, S("TESTING KEY"), S("PRIVATE KEY"));
}

/* Not in Go: a key that does not parse fails the test, where Go's parseKey
 * panics. */
static RsaPrivateKey *parse_key(TestingT *t, Alloc *a, Str s) {
    Slice rest;
    PemBlock *p = pem_decode(
        a, slice_from((void *)(uintptr_t)s.p, s.len, s.len, TYPE_BYTE), &rest);
    if (p == NULL)
        testing_t_fatal_v(t, "no PEM block");
    Error err = BURROW_NO_ERROR;
    RsaPrivateKey *k = x509_parse_pkcs1_private_key(a, p->bytes, &err);
    if (k == NULL)
        testing_t_fatalf_v(t, "%s", error_text(err));
    return k;
}

static const char small_key_pem[] =
    "-----BEGIN RSA TESTING KEY-----\n"
    "MIICXQIBAAKBgQDMrln6XoAa3Rjts+kRi5obbP86qSf/562RcuDO+yMXeTLHfi4M\n"
    "8ubyhoFY+UKBCGBLmmTO7ikbvQgdipkT3xVkU8nM3XTW4sxrnw0X5QXsl4PGlMo0\n"
    "5UufxYyQxe7bbjuwFz2XnN6Jz4orpOfO0s36/KVHj9lZRl+REpr/Jy+nJQIDAQAB\n"
    "AoGAJ9WEwGO01cWSzOwXH2mGX/EKCQ4TsUuS7XwogU/B6BcXyVhmuPFq/ecsdDbq\n"
    "ePc62mvdU6JpELNsyWcIXKQtYsRgJHxNS+KJkCQIq6YeiAWRG0XL6q+qVj+HtT8a\n"
    "1Qrmul9ZBd23Y9wLF8pg/xWDQYvb8DPAb/xJ0e/KEBZcWU8CQQDXFCFCGpCfwyxY\n"
    "Cq8G/3B94D9UYwk5mK6jRIH5m8LbaX9bKKetf8+If8TWVgeuiRjjN4WEQ78lPoSg\n"
    "3Fsz2qs3AkEA85/JCudNUf2FnY+T6h1c/2SWekZiZ1NS4lCh/C7iYuAN3oa8zGkf\n"
    "gjjR5e0+Z8rUAcZkTukxyLLaNqy6rs9GgwJAVR6pXvEGhcQHe7yWso1LpvWl+q7L\n"
    "StkrXIBTdEb54j4pYhl/6wFnUB1I+I7JsYCeseYaWFM7hfDtKoCrM6V6FwJBANxh\n"
    "KmfmnJcSkw/YlaEuNrYAs+6gRNvbEBsRfba2Yqu2qlUl5Ruz7IDMDXPEjLMvU2DX\n"
    "ql2HrTU0NRlIXwdLESkCQQDGJ54H6WK1eE1YvtxCaLm28zmogcFlvc21pym+PpM1\n"
    "bXVL8iKLrG91IYQByUHZIn3WVAd2bfi4MfKagRt0ggd4\n"
    "-----END RSA TESTING KEY-----";

static void hpke_kem_test(void *env, TestingT *t) {
    const HpkeKEM *kem = env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error ret_err;

    Error err = BURROW_NO_ERROR;
    HpkePrivateKey *k = hpke_kem_generate_key(kem, a, &err);
    expect_no_err(t, err);
    expect_no_err(t, ERR_RET(hpke_kem_derive_key_pair(kem, a, zeros(a, 64), &ret_err)));
    err = BURROW_NO_ERROR;
    Slice kb = hpke_private_key_bytes(k, a, &err);
    expect_no_err(t, err);
    expect_no_err(t, ERR_RET(hpke_kem_new_private_key(kem, a, kb, &ret_err)));
    expect_no_err(t,
                  ERR_RET(hpke_kem_new_public_key(
                      kem, a, hpke_public_key_bytes(hpke_private_key_public_key(k), a),
                      &ret_err)));
    err = BURROW_NO_ERROR;
    Slice c =
        hpke_seal(a, hpke_private_key_public_key(k), hpke_hkdfsha256(),
                  hpke_aes128_gcm(), slice_nil(TYPE_BYTE), slice_nil(TYPE_BYTE), &err);
    expect_no_err(t, err);
    err = BURROW_NO_ERROR;
    (void)hpke_open(a, k, hpke_hkdfsha256(), hpke_aes128_gcm(), slice_nil(TYPE_BYTE), c,
                    &err);
    expect_no_err(t, err);
    arena_free(&ar);
}

static void test_fips140_only(TestingT *t) {
    if (!fips140_enforced())
        testing_t_fatal_v(t, "FIPS 140-only mode not enforced");
    testing_t_logf_v(t, "GODEBUG=fips140=only enabled");
    burrow__fips140_reset_service_indicator();

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error ret_err;
    Error err = BURROW_NO_ERROR;

    CipherBlock aes_block = aes_new_cipher(a, zeros(a, 16), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    Only o = {a, aes_block, {&block_wrap_vt, &aes_block}, zeros(a, AES_BLOCK_SIZE),
              0, NULL};

    (void)cipher_new_cbc_encrypter(a, o.aes_block, o.iv);
    expect_panic(t, new_cbc_encrypter_not_aes, &o);
    (void)cipher_new_cbc_decrypter(a, o.aes_block, o.iv);
    expect_panic(t, new_cbc_decrypter_not_aes, &o);

    expect_panic(t, new_cfb_encrypter, &o);
    expect_panic(t, new_cfb_decrypter, &o);

    (void)cipher_new_ctr(a, o.aes_block, o.iv);
    expect_panic(t, new_ctr_not_aes, &o);

    expect_panic(t, new_ofb, &o);

    expect_err(t, ERR_RET(cipher_new_gcm(a, aes_block, &ret_err)));
    expect_err(t, ERR_RET(cipher_new_gcm_with_nonce_size(a, aes_block, 12, &ret_err)));
    expect_err(t, ERR_RET(cipher_new_gcm_with_tag_size(a, aes_block, 12, &ret_err)));
    expect_no_err(t, ERR_RET(cipher_new_gcm_with_random_nonce(a, aes_block, &ret_err)));

    expect_err(t, ERR_RET(des_new_cipher(a, zeros(a, 8), &ret_err)));
    expect_err(t, ERR_RET(des_new_triple_des_cipher(a, zeros(a, 24), &ret_err)));

    expect_err(t, ERR_RET(rc4_new_cipher(a, zeros(a, 16), &ret_err)));

    expect_err(t, ERR_RET(chacha20poly1305_new(a, zeros(a, CHACHA20POLY1305_KEY_SIZE),
                                               &ret_err)));
    expect_err(t, ERR_RET(chacha20poly1305_new_x(a, zeros(a, CHACHA20POLY1305_KEY_SIZE),
                                                 &ret_err)));

    expect_panic(t, md5_new_sum, &o);
    expect_err(t, ERR_RET(hash_write(md5_new(a), zeros(a, 16), &ret_err)));
    expect_panic(t, md5_sum_foo, &o);

    expect_panic(t, sha1_new_sum, &o);
    expect_err(t, ERR_RET(hash_write(sha1_new(a), zeros(a, 16), &ret_err)));
    expect_panic(t, sha1_sum_foo, &o);

    for (int i = 0; i < N_APPROVED; i++)
        (void)hash_sum(a, crypto_hash_new(approved_hashes[i].h, a),
                       slice_nil(TYPE_BYTE));
    for (int i = 0; i < N_NON_APPROVED; i++) {
        o.h = non_approved_hashes[i].h;
        expect_panic(t, hash_new_sum, &o);
    }

    expect_err(t, ERR_RET(pbkdf2_key(a, sha256_new, S("password"), zeros(a, 16), 1, 10,
                                     &ret_err)));
    expect_err(t, ERR_RET(pbkdf2_key(a, sha256_new, S("password"), zeros(a, 10), 1, 14,
                                     &ret_err)));
    for (int i = 0; i < N_NON_APPROVED; i++)
        expect_err(t,
                   ERR_RET(pbkdf2_key(a, non_approved_hashes[i].new_hash, S("password"),
                                      zeros(a, 16), 1, 14, &ret_err)));
    for (int i = 0; i < N_APPROVED; i++)
        expect_no_err(t,
                      ERR_RET(pbkdf2_key(a, approved_hashes[i].new_hash, S("password"),
                                         zeros(a, 16), 1, 14, &ret_err)));

    expect_panic(t, hmac_new_short_key, &o);
    for (int i = 0; i < N_NON_APPROVED; i++) {
        o.h_new = non_approved_hashes[i].new_hash;
        expect_panic(t, hmac_new_hash, &o);
    }
    for (int i = 0; i < N_APPROVED; i++)
        (void)hmac_new(a, approved_hashes[i].new_hash, zeros(a, 16));

    Slice nil = slice_nil(TYPE_BYTE);
    expect_err(
        t, ERR_RET(hkdf_key(a, sha256_new, zeros(a, 10), nil, S(""), 16, &ret_err)));
    for (int i = 0; i < N_NON_APPROVED; i++)
        expect_err(t, ERR_RET(hkdf_key(a, non_approved_hashes[i].new_hash, zeros(a, 16),
                                       nil, S(""), 16, &ret_err)));
    for (int i = 0; i < N_APPROVED; i++)
        expect_no_err(t, ERR_RET(hkdf_key(a, approved_hashes[i].new_hash, zeros(a, 16),
                                          nil, S(""), 16, &ret_err)));

    expect_err(t, ERR_RET(hkdf_extract(a, sha256_new, zeros(a, 10), nil, &ret_err)));
    for (int i = 0; i < N_NON_APPROVED; i++)
        expect_err(t, ERR_RET(hkdf_extract(a, non_approved_hashes[i].new_hash,
                                           zeros(a, 16), nil, &ret_err)));
    for (int i = 0; i < N_APPROVED; i++)
        expect_no_err(t, ERR_RET(hkdf_extract(a, approved_hashes[i].new_hash,
                                              zeros(a, 16), nil, &ret_err)));

    expect_err(t,
               ERR_RET(hkdf_expand(a, sha256_new, zeros(a, 10), S(""), 16, &ret_err)));
    for (int i = 0; i < N_NON_APPROVED; i++)
        expect_err(t, ERR_RET(hkdf_expand(a, non_approved_hashes[i].new_hash,
                                          zeros(a, 16), S(""), 16, &ret_err)));
    for (int i = 0; i < N_APPROVED; i++)
        expect_no_err(t, ERR_RET(hkdf_expand(a, approved_hashes[i].new_hash,
                                             zeros(a, 16), S(""), 16, &ret_err)));

    expect_err(t, ERR_RET(crypto_rand_prime(a, crypto_rand_reader, 10, &ret_err)));

    DsaParameters dsa_params;
    memset(&dsa_params, 0, sizeof dsa_params);
    expect_err(
        t, dsa_generate_parameters(&dsa_params, a, crypto_rand_reader, DSA_L1024N160));
    DsaPrivateKey dsa_priv;
    memset(&dsa_priv, 0, sizeof dsa_priv);
    expect_err(t, dsa_generate_key(&dsa_priv, a, crypto_rand_reader));
    BigInt *dsa_s = NULL;
    expect_err(t, ERR_RET(dsa_sign(a, crypto_rand_reader, &dsa_priv, zeros(a, 16),
                                   &dsa_s, &ret_err)));
    expect_panic(t, dsa_verify_empty, &o);

    expect_err(t, ERR_RET(ecdh_curve_generate_key(ecdh_x25519(), a, crypto_rand_reader,
                                                  &ret_err)));
    expect_err(t, ERR_RET(ecdh_curve_new_private_key(ecdh_x25519(), a, zeros(a, 32),
                                                     &ret_err)));
    expect_err(t, ERR_RET(ecdh_curve_new_public_key(ecdh_x25519(), a, zeros(a, 32),
                                                    &ret_err)));
    const EcdhCurve *ecdh_curves[] = {ecdh_p256(), ecdh_p384(), ecdh_p521()};
    for (int i = 0; i < 3; i++) {
        const EcdhCurve *curve = ecdh_curves[i];
        expect_err_if_custom_rand(
            t, ERR_RET(ecdh_curve_generate_key(curve, a, reader_wrap, &ret_err)));
        err = BURROW_NO_ERROR;
        EcdhPrivateKey *k = ecdh_curve_generate_key(curve, a, crypto_rand_reader, &err);
        if (k == NULL)
            testing_t_fatalf_v(t, "%s", error_text(err));
        expect_no_err(t, ERR_RET(ecdh_curve_new_private_key(
                             curve, a, ecdh_private_key_bytes(k, a), &ret_err)));
        expect_no_err(
            t, ERR_RET(ecdh_curve_new_public_key(
                   curve, a, ecdh_public_key_bytes(ecdh_private_key_public_key(k), a),
                   &ret_err)));
    }

    EllipticCurve elliptic_curves[] = {elliptic_p256(), elliptic_p384(),
                                       elliptic_p521()};
    CryptoSignerOpts no_opts = {NULL, NULL};
    for (int i = 0; i < 3; i++) {
        EllipticCurve curve = elliptic_curves[i];
        expect_err_if_custom_rand(
            t, ERR_RET(ecdsa_generate_key(a, curve, reader_wrap, &ret_err)));
        err = BURROW_NO_ERROR;
        EcdsaPrivateKey *k = ecdsa_generate_key(a, curve, crypto_rand_reader, &err);
        if (k == NULL)
            testing_t_fatalf_v(t, "%s", error_text(err));

        BigInt *s = NULL;
        expect_err_if_custom_rand(
            t, ERR_RET(ecdsa_private_key_sign(k, a, reader_wrap, zeros(a, 32), no_opts,
                                              &ret_err)));
        expect_err_if_custom_rand(
            t, ERR_RET(ecdsa_sign_asn1(a, reader_wrap, k, zeros(a, 32), &ret_err)));
        expect_err_if_custom_rand(
            t, ERR_RET(ecdsa_sign(a, reader_wrap, k, zeros(a, 32), &s, &ret_err)));
        expect_no_err(t,
                      ERR_RET(ecdsa_private_key_sign(k, a, crypto_rand_reader,
                                                     zeros(a, 32), no_opts, &ret_err)));
        expect_no_err(t, ERR_RET(ecdsa_sign_asn1(a, crypto_rand_reader, k, zeros(a, 32),
                                                 &ret_err)));
        expect_no_err(t, ERR_RET(ecdsa_sign(a, crypto_rand_reader, k, zeros(a, 32), &s,
                                            &ret_err)));

        for (int j = 0; j < N_NON_APPROVED; j++) {
            CryptoHash h = non_approved_hashes[j].h;
            expect_err(t, ERR_RET(ecdsa_private_key_sign(
                              k, a, nil_reader, zeros(a, crypto_hash_size(h)),
                              crypto_hash_as_signer_opts(&h), &ret_err)));
        }
        for (int j = 0; j < N_APPROVED; j++) {
            CryptoHash h = approved_hashes[j].h;
            expect_no_err(t, ERR_RET(ecdsa_private_key_sign(
                                 k, a, nil_reader, zeros(a, crypto_hash_size(h)),
                                 crypto_hash_as_signer_opts(&h), &ret_err)));
        }
    }
    EllipticCurveParams custom_curve;
    memset(&custom_curve, 0, sizeof custom_curve);
    custom_curve.name = S("custom");
    custom_curve.p = big_new_int(a, 1);
    expect_err(t, ERR_RET(ecdsa_generate_key(
                      a, elliptic_curve_params_as_elliptic_curve(&custom_curve),
                      crypto_rand_reader, &ret_err)));

    Ed25519PrivateKey ed25519_key;
    err = BURROW_NO_ERROR;
    (void)ed25519_generate_key(a, crypto_rand_reader, &ed25519_key, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    CryptoHash hash0 = 0;
    CryptoHash sha512 = CRYPTO_SHA512;
    expect_no_err(t, ERR_RET(ed25519_private_key_sign(
                         ed25519_key, a, nil_reader, zeros(a, 32),
                         crypto_hash_as_signer_opts(&hash0), &ret_err)));
    expect_no_err(t, ERR_RET(ed25519_private_key_sign(
                         ed25519_key, a, nil_reader, zeros(a, 64),
                         crypto_hash_as_signer_opts(&sha512), &ret_err)));
    /* ed25519ctx is not allowed (but ed25519ph with context is). */
    Ed25519Options ctx_opts = {0, S("test")};
    expect_err(t, ERR_RET(ed25519_private_key_sign(
                      ed25519_key, a, nil_reader, zeros(a, 32),
                      ed25519_options_as_signer_opts(&ctx_opts), &ret_err)));
    Ed25519Options ph_ctx_opts = {CRYPTO_SHA512, S("test")};
    expect_no_err(t, ERR_RET(ed25519_private_key_sign(
                         ed25519_key, a, nil_reader, zeros(a, 64),
                         ed25519_options_as_signer_opts(&ph_ctx_opts), &ret_err)));
    Ed25519Options ph_opts = {CRYPTO_SHA512, BURROW_STR_EMPTY};
    expect_no_err(t, ERR_RET(ed25519_private_key_sign(
                         ed25519_key, a, nil_reader, zeros(a, 64),
                         ed25519_options_as_signer_opts(&ph_opts), &ret_err)));

    expect_err(t, ERR_RET(rsa_generate_multi_prime_key(a, crypto_rand_reader, 3, 2048,
                                                       &ret_err)));
    expect_err(t, ERR_RET(rsa_generate_key(a, crypto_rand_reader, 1024, &ret_err)));
    expect_err(t, ERR_RET(rsa_generate_key(a, crypto_rand_reader, 2049, &ret_err)));
    expect_err_if_custom_rand(
        t, ERR_RET(rsa_generate_key(a, reader_wrap, 2048, &ret_err)));
    err = BURROW_NO_ERROR;
    RsaPrivateKey *rsa_key = rsa_generate_key(a, crypto_rand_reader, 2048, &err);
    expect_no_err(t, err);

    RsaPrivateKey *small_key =
        parse_key(t, a, testing_key(a, str_from_cstr(small_key_pem)));

    CryptoHash sha256 = CRYPTO_SHA256;
    CryptoHash sha1 = CRYPTO_SHA1;
    expect_no_err(t, ERR_RET(rsa_private_key_sign(
                         rsa_key, a, crypto_rand_reader, zeros(a, 32),
                         crypto_hash_as_signer_opts(&sha256), &ret_err)));
    expect_err(t, ERR_RET(rsa_private_key_sign(
                      small_key, a, crypto_rand_reader, zeros(a, 32),
                      crypto_hash_as_signer_opts(&sha256), &ret_err)));
    expect_err(
        t, ERR_RET(rsa_private_key_sign(rsa_key, a, crypto_rand_reader, zeros(a, 20),
                                        crypto_hash_as_signer_opts(&sha1), &ret_err)));
    /* rand is always ignored for PKCS1v15 signing */
    expect_no_err(t, ERR_RET(rsa_private_key_sign(rsa_key, a, reader_wrap, zeros(a, 32),
                                                  crypto_hash_as_signer_opts(&sha256),
                                                  &ret_err)));

    err = BURROW_NO_ERROR;
    Slice sig_pkcs1v15 = rsa_sign_pkcs1_v15(a, crypto_rand_reader, rsa_key,
                                            CRYPTO_SHA256, zeros(a, 32), &err);
    expect_no_err(t, err);
    expect_err(t, ERR_RET(rsa_sign_pkcs1_v15(a, crypto_rand_reader, small_key,
                                             CRYPTO_SHA256, zeros(a, 32), &ret_err)));
    expect_err(t, ERR_RET(rsa_sign_pkcs1_v15(a, crypto_rand_reader, rsa_key,
                                             CRYPTO_SHA1, zeros(a, 20), &ret_err)));
    /* rand is always ignored for PKCS1v15 signing */
    expect_no_err(t, ERR_RET(rsa_sign_pkcs1_v15(a, reader_wrap, rsa_key, CRYPTO_SHA256,
                                                zeros(a, 32), &ret_err)));
    expect_err(t, ERR_RET(rsa_sign_pkcs1_v15(a, crypto_rand_reader, rsa_key, 0,
                                             zeros(a, 32), &ret_err)));

    expect_no_err(t, rsa_verify_pkcs1_v15(&rsa_key->public_key, CRYPTO_SHA256,
                                          zeros(a, 32), sig_pkcs1v15));
    expect_err(t, rsa_verify_pkcs1_v15(&small_key->public_key, CRYPTO_SHA256,
                                       zeros(a, 32), sig_pkcs1v15));
    expect_err(t, rsa_verify_pkcs1_v15(&rsa_key->public_key, CRYPTO_SHA1, zeros(a, 20),
                                       sig_pkcs1v15));
    expect_err(
        t, rsa_verify_pkcs1_v15(&rsa_key->public_key, 0, zeros(a, 32), sig_pkcs1v15));

    err = BURROW_NO_ERROR;
    Slice sig_pss = rsa_sign_pss(a, crypto_rand_reader, rsa_key, CRYPTO_SHA256,
                                 zeros(a, 32), NULL, &err);
    expect_no_err(t, err);
    expect_err(t, ERR_RET(rsa_sign_pss(a, crypto_rand_reader, small_key, CRYPTO_SHA256,
                                       zeros(a, 32), NULL, &ret_err)));
    expect_err(t, ERR_RET(rsa_sign_pss(a, crypto_rand_reader, rsa_key, CRYPTO_SHA1,
                                       zeros(a, 20), NULL, &ret_err)));
    expect_err(t, ERR_RET(rsa_sign_pss(a, reader_wrap, rsa_key, CRYPTO_SHA256,
                                       zeros(a, 32), NULL, &ret_err)));

    expect_no_err(t, rsa_verify_pss(&rsa_key->public_key, CRYPTO_SHA256, zeros(a, 32),
                                    sig_pss, NULL));
    expect_err(t, rsa_verify_pss(&small_key->public_key, CRYPTO_SHA256, zeros(a, 32),
                                 sig_pss, NULL));
    expect_err(t, rsa_verify_pss(&rsa_key->public_key, CRYPTO_SHA1, zeros(a, 20),
                                 sig_pss, NULL));

    Slice ct;
    err = BURROW_NO_ERROR;
    MlkemDecapsulationKey768 *k = mlkem_generate_key768(a, &err);
    expect_no_err(t, err);
    expect_err(t, ERR_RET(mlkemtest_encapsulate768(
                      mlkem_decapsulation_key768_encapsulation_key(k), a, zeros(a, 32),
                      &ct, &ret_err)));
    err = BURROW_NO_ERROR;
    MlkemDecapsulationKey1024 *k1024 = mlkem_generate_key1024(a, &err);
    expect_no_err(t, err);
    expect_err(t, ERR_RET(mlkemtest_encapsulate1024(
                      mlkem_decapsulation_key1024_encapsulation_key(k1024), a,
                      zeros(a, 32), &ct, &ret_err)));

    const HpkeKEM *kems[] = {
        hpke_dhkem(ecdh_p256()), hpke_dhkem(ecdh_p384()), hpke_dhkem(ecdh_p521()),
        hpke_mlkem768(),         hpke_mlkem1024(),        hpke_mlkem768_p256(),
        hpke_mlkem1024_p384(),   hpke_mlkem768_x25519(), /* allowed as hybrid */
    };
    for (size_t i = 0; i < sizeof kems / sizeof kems[0]; i++) {
        Str name = fmt_sprintf_v(a, "HKPE KEM %04x", hpke_kem_id(kems[i]));
        testing_t_run(
            t, name,
            BURROW_FN(TestingTFunc, hpke_kem_test, (void *)(uintptr_t)kems[i]));
    }
    const HpkeKEM *x25519 = hpke_dhkem(ecdh_x25519());
    expect_err(t, ERR_RET(hpke_kem_generate_key(x25519, a, &ret_err)));
    expect_err(t, ERR_RET(hpke_kem_derive_key_pair(x25519, a, zeros(a, 64), &ret_err)));
    expect_err(t, ERR_RET(hpke_kem_new_private_key(x25519, a, zeros(a, 32), &ret_err)));
    expect_err(t, ERR_RET(hpke_kem_new_public_key(x25519, a, zeros(a, 32), &ret_err)));
    err = BURROW_NO_ERROR;
    HpkePrivateKey *hpke_k = hpke_kem_generate_key(hpke_mlkem768(), a, &err);
    expect_no_err(t, err);
    expect_err(
        t, ERR_RET(hpke_seal(a, hpke_private_key_public_key(hpke_k), hpke_hkdfsha256(),
                             hpke_cha_cha20_poly1305(), nil, nil, &ret_err)));
    expect_err(
        t, ERR_RET(hpke_open(a, hpke_k, hpke_hkdfsha256(), hpke_cha_cha20_poly1305(),
                             nil, zeros(a, 2000), &ret_err)));

    /* fips140=only mode should prevent any operation that would make the FIPS
     * 140-3 module set its service indicator to false. */
    if (!burrow__fips140_service_indicator())
        testing_t_errorf_v(t, "service indicator not set");
    arena_free(&ar);
}

static void custom_rand_0(void *env, TestingT *t) {
    (void)env;
    setenv_godebug(t, ",cryptocustomrand=0");
    cryptocustomrand = "0";
    test_fips140_only(t);
}

static void custom_rand_1(void *env, TestingT *t) {
    (void)env;
    setenv_godebug(t, ",cryptocustomrand=1");
    cryptocustomrand = "1";
    test_fips140_only(t);
}

static void TestFIPS140Only(TestingT *t) {
    if (!fips140_enforced()) {
        Arena ar;
        arena_init(&ar, NULL, 0);
        fipstest_rerun(t, arena_allocator(&ar), "only", NULL);
        arena_free(&ar);
        return;
    }
    testing_t_run(t, S("cryptocustomrand=0"),
                  BURROW_FN(TestingTFunc, custom_rand_0, NULL));
    testing_t_run(t, S("cryptocustomrand=1"),
                  BURROW_FN(TestingTFunc, custom_rand_1, NULL));
}

#define TESTS(X) X(TestFIPS140Only)

TESTING_MAIN(TESTS)
