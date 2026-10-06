/* Derived from Go's src/crypto/crypto_test.go.
 * Go source: go1.27.1.
 *
 * TestSignMessage there signs with RSA and parses the result with crypto/x509,
 * neither of which burrow has yet, so it waits for them. The signers here are
 * test doubles that record what they were given, which checks the same thing:
 * whether the message or its digest reached the key.
 * TestDisallowedAssemblyInstructions checks Go's assembly and has nothing to
 * check in C.
 *
 * Everything below TestMLDSAMu is burrow's.
 *
 * Copyright 2024 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"
#include "fatal.h"

#include "burrow/burrow.h"
#include "burrow/bytes.h"
#include "burrow/crypto.h"
#include "burrow/crypto/sha256.h"
#include "burrow/encoding/hex.h"

#include <stdint.h>
#include <string.h>

static Slice text(const char *s) {
    Int n = (Int)strlen(s);
    return slice_from((void *)(uintptr_t)s, n, n, TYPE_BYTE);
}

static void TestRegisterHashLimits(TestingT *t) {
    static const CryptoHash bad[] = {0, 21, CRYPTO_MLDSA_MU};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        EXPECT_PANIC(crypto_register_hash(bad[i], sha256_new));
        if (!fatal_did_catch)
            testing_t_errorf_v(t, "RegisterHash(%d) did not panic", (Int)bad[i]);
    }
}

static void TestMLDSAMu(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    CryptoHash h = CRYPTO_MLDSA_MU;
    if (crypto_hash_size(h) != 64)
        testing_t_errorf_v(t, "Size() = %d, want 64", crypto_hash_size(h));
    Str want = BURROW_S("ML-DSA \xce\xbc message representative");
    if (!str_eq(crypto_hash_string(h, a), want))
        testing_t_errorf_v(t, "String() = %q, want %q", crypto_hash_string(h, a), want);
    if (crypto_hash_available(h))
        testing_t_errorf_v(t, "Available() = true, want false");
    if (crypto_hash_hash_func(h) != h)
        testing_t_errorf_v(t, "HashFunc() = %d, want %d", (Int)crypto_hash_hash_func(h),
                           (Int)h);
    EXPECT_PANIC(crypto_hash_new(h, a));
    if (!fatal_did_catch)
        testing_t_errorf_v(t, "New() did not panic");
    arena_free(&ar);
}

/* Each hash's name and the digest of "abc", from Go's crypto.Hash with the
 * standard library's hashes imported. NULL is a hash Go does not have. */
typedef struct HashTest {
    CryptoHash h;
    const char *name;
    Int size;
    const char *abc;
} HashTest;

static const HashTest hashes[] = {
    {CRYPTO_MD4, "MD4", 16, NULL},
    {CRYPTO_MD5, "MD5", 16, "900150983cd24fb0d6963f7d28e17f72"},
    {CRYPTO_SHA1, "SHA-1", 20, "a9993e364706816aba3e25717850c26c9cd0d89d"},
    {CRYPTO_SHA224, "SHA-224", 28,
     "23097d223405d8228642a477bda255b32aadbce4bda0b3f7e36c9da7"},
    {CRYPTO_SHA256, "SHA-256", 32,
     "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
    {CRYPTO_SHA384, "SHA-384", 48,
     "cb00753f45a35e8bb5a03d699ac65007272c32ab0eded163"
     "1a8b605a43ff5bed8086072ba1e7cc2358baeca134c825a7"},
    {CRYPTO_SHA512, "SHA-512", 64,
     "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
     "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f"},
    {CRYPTO_MD5SHA1, "MD5+SHA1", 36, NULL},
    {CRYPTO_RIPEMD160, "RIPEMD-160", 20, NULL},
    {CRYPTO_SHA3_224, "SHA3-224", 28,
     "e642824c3f8cf24ad09234ee7d3c766fc9a3a5168d0c94ad73b46fdf"},
    {CRYPTO_SHA3_256, "SHA3-256", 32,
     "3a985da74fe225b2045c172d6bd390bd855f086e3e9d525b46bfe24511431532"},
    {CRYPTO_SHA3_384, "SHA3-384", 48,
     "ec01498288516fc926459f58e2c6ad8df9b473cb0fc08c25"
     "96da7cf0e49be4b298d88cea927ac7f539f1edf228376d25"},
    {CRYPTO_SHA3_512, "SHA3-512", 64,
     "b751850b1a57168a5693cd924b6b096e08f621827444f70d884f5d0240d2712e"
     "10e116e9192af3c91a7ec57647e3934057340b4cf408d5a56592f8274eec53f0"},
    {CRYPTO_SHA512_224, "SHA-512/224", 28,
     "4634270f707b6a54daae7530460842e20e37ed265ceee9a43e8924aa"},
    {CRYPTO_SHA512_256, "SHA-512/256", 32,
     "53048e2681941ef99b2e29b76b4c7dabe4c2d0c634fc6d46e0e2f13107e7af23"},
    {CRYPTO_BLAKE2_S256, "BLAKE2s-256", 32, NULL},
    {CRYPTO_BLAKE2_B256, "BLAKE2b-256", 32, NULL},
    {CRYPTO_BLAKE2_B384, "BLAKE2b-384", 48, NULL},
    {CRYPTO_BLAKE2_B512, "BLAKE2b-512", 64, NULL},
};

#define NHASHES ((Int)(sizeof hashes / sizeof hashes[0]))

static void TestHashes(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < NHASHES; i++) {
        const HashTest *ht = &hashes[i];
        Str name = str_from_cstr(ht->name);
        if (!str_eq(crypto_hash_string(ht->h, a), name))
            testing_t_errorf_v(t, "Hash(%d).String() = %q, want %q", (Int)ht->h,
                               crypto_hash_string(ht->h, a), name);
        if (crypto_hash_size(ht->h) != ht->size)
            testing_t_errorf_v(t, "%s.Size() = %d, want %d", name,
                               crypto_hash_size(ht->h), ht->size);
        if (crypto_hash_hash_func(ht->h) != ht->h)
            testing_t_errorf_v(t, "%s.HashFunc() is not itself", name);
        bool want = ht->abc != NULL;
        if (crypto_hash_available(ht->h) != want) {
            testing_t_errorf_v(t, "%s.Available() = %t, want %t", name, !want, want);
            continue;
        }
        if (!want) {
            Str msg = fmt_sprintf_v(
                a, "crypto: requested hash function unavailable: %s", name);
            EXPECT_PANIC(crypto_hash_new(ht->h, a));
            if (!fatal_did_catch || !str_eq(str_from_cstr(fatal_caught), msg))
                testing_t_errorf_v(t, "%s.New() panicked with %q, want %q", name,
                                   str_from_cstr(fatal_caught), msg);
            continue;
        }
        Hash d = crypto_hash_new(ht->h, a);
        if (hash_size(d) != ht->size)
            testing_t_errorf_v(t, "%s.New().Size() = %d, want %d", name, hash_size(d),
                               ht->size);
        hash_write(d, text("abc"), NULL);
        Str got = hex_encode_to_string(a, hash_sum(a, d, slice_nil(TYPE_BYTE)));
        if (!str_eq(got, str_from_cstr(ht->abc)))
            testing_t_errorf_v(t, "%s(\"abc\") = %s, want %s", name, got,
                               str_from_cstr(ht->abc));
    }
    arena_free(&ar);
}

static void TestUnknownHash(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    static const struct {
        CryptoHash h;
        const char *want;
    } tests[] = {
        {0, "unknown hash value 0"},
        {21, "unknown hash value 21"},
        {42, "unknown hash value 42"},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Str got = crypto_hash_string(tests[i].h, a);
        if (!str_eq(got, str_from_cstr(tests[i].want)))
            testing_t_errorf_v(t, "Hash(%d).String() = %q, want %q", (Int)tests[i].h,
                               got, str_from_cstr(tests[i].want));
        if (crypto_hash_available(tests[i].h))
            testing_t_errorf_v(t, "Hash(%d).Available() = true", (Int)tests[i].h);
        EXPECT_PANIC(crypto_hash_size(tests[i].h));
        if (!fatal_did_catch ||
            strcmp(fatal_caught, "crypto: Size of unknown hash function") != 0)
            testing_t_errorf_v(t, "Hash(%d).Size() panicked with %q", (Int)tests[i].h,
                               str_from_cstr(fatal_caught));
    }
    arena_free(&ar);
}

static void TestRegisterHash(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    /* A hash burrow does not have, standing in for one a program brings. */
    crypto_register_hash(CRYPTO_MD4, sha256_new);
    if (!crypto_hash_available(CRYPTO_MD4))
        testing_t_errorf_v(t, "MD4 not available after RegisterHash");
    else if (hash_size(crypto_hash_new(CRYPTO_MD4, a)) != 32)
        testing_t_errorf_v(t, "MD4.New() is not the registered function");
    crypto_register_hash(CRYPTO_MD4, NULL);
    if (crypto_hash_available(CRYPTO_MD4))
        testing_t_errorf_v(t, "MD4 still available after registering NULL");

    /* Registering over one of burrow's replaces it, and NULL hides it. */
    crypto_register_hash(CRYPTO_SHA256, NULL);
    if (crypto_hash_available(CRYPTO_SHA256))
        testing_t_errorf_v(t, "SHA-256 still available after registering NULL");
    crypto_register_hash(CRYPTO_SHA256, sha256_new224);
    if (hash_size(crypto_hash_new(CRYPTO_SHA256, a)) != 28)
        testing_t_errorf_v(t, "SHA-256.New() is not the registered function");
    crypto_register_hash(CRYPTO_SHA256, sha256_new);
    if (hash_size(crypto_hash_new(CRYPTO_SHA256, a)) != 32)
        testing_t_errorf_v(t, "SHA-256.New() was not put back");
    arena_free(&ar);
}

/* A Signer that keeps a copy of what reached sign. */
typedef struct FakeSigner {
    Alloc *a;
    Slice got;
    CryptoHash opts;
    int calls;
} FakeSigner;

static CryptoPublicKey fake_public(void *self) {
    (void)self;
    return (CryptoPublicKey){0};
}

static Slice fake_sign(void *self, Alloc *a, IoReader rand, Slice digest,
                       CryptoSignerOpts opts, Error *err) {
    (void)rand;
    FakeSigner *s = self;
    s->got = bytes_clone(s->a, digest);
    s->opts = opts.vt->hash_func(opts.data);
    s->calls++;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return bytes_clone(a, text("signature"));
}

static const CryptoSignerVT fake_signer_vt = {NULL, fake_public, fake_sign};

/* A MessageSigner, found through the SignMessage in its type's method set. */
#define MSG_SIGNER_FIELDS(F, T) F(T, Int, Calls, "")

BURROW_STRUCT_DECL(MsgSigner, MSG_SIGNER_FIELDS);

static Slice msg_signer_sign_message(MsgSigner *s, CryptoAllocArg a, IoReader rand,
                                     Bytes msg, CryptoSignerOpts opts,
                                     CryptoErrorArg err) {
    (void)rand;
    (void)opts;
    s->Calls++;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return bytes_clone(a, msg);
}

#define MSG_SIGNER_METHODS(M, T)                                                       \
    M(T, SignMessage, msg_signer_sign_message, CRYPTO_SIG_SIGN_MESSAGE)

BURROW_STRUCT_DEFINE_METHODS(MsgSigner, MSG_SIGNER_FIELDS, MSG_SIGNER_METHODS);

static Slice msg_signer_sign(void *self, Alloc *a, IoReader rand, Slice digest,
                             CryptoSignerOpts opts, Error *err) {
    (void)self;
    (void)a;
    (void)rand;
    (void)digest;
    (void)opts;
    BURROW_OUT(err, errors_new(error_allocator(), BURROW_S("sign called")));
    return slice_nil(TYPE_BYTE);
}

static const CryptoSignerVT msg_signer_vt = {TYPE_OF(MsgSigner), fake_public,
                                             msg_signer_sign};

static void TestSignMessageDigest(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    FakeSigner fs = {a, {0}, 0, 0};
    CryptoSigner s = {&fake_signer_vt, &fs};
    CryptoHash h = CRYPTO_SHA256;
    IoReader rand = {0};
    Error err = BURROW_NO_ERROR;
    Slice sig = crypto_sign_message(a, s, rand, text("abc"),
                                    crypto_hash_as_signer_opts(&h), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "SignMessage: %v", err);
    if (!bytes_equal(sig, text("signature")))
        testing_t_errorf_v(t, "SignMessage = %q, want the signer's", sig);
    Str got = hex_encode_to_string(a, fs.got);
    if (fs.calls != 1 || !str_eq(got, str_from_cstr(hashes[4].abc)))
        testing_t_errorf_v(t, "sign got %s, want the SHA-256 of the message", got);
    if (fs.opts != CRYPTO_SHA256)
        testing_t_errorf_v(t, "sign got opts %d, want SHA-256", (Int)fs.opts);

    /* Zero says the message is not to be hashed, and it goes through as is. */
    CryptoHash none = 0;
    crypto_sign_message(a, s, rand, text("abc"), crypto_hash_as_signer_opts(&none),
                        &err);
    if (BURROW_FAILED(err) || !bytes_equal(fs.got, text("abc")))
        testing_t_errorf_v(t, "with no hash, sign got %q, %v", fs.got, err);
    arena_free(&ar);
}

static void TestSignMessageUnavailable(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    FakeSigner fs = {a, {0}, 0, 0};
    CryptoSigner s = {&fake_signer_vt, &fs};
    CryptoHash h = CRYPTO_MD4;
    IoReader rand = {0};
    Error err = BURROW_NO_ERROR;
    Slice sig = crypto_sign_message(a, s, rand, text("abc"),
                                    crypto_hash_as_signer_opts(&h), &err);
    if (!BURROW_FAILED(err))
        testing_t_fatalf_v(t, "SignMessage with MD4 succeeded");
    Str want = BURROW_S("crypto: requested hash function unavailable: MD4");
    if (!str_eq(error_text(err), want))
        testing_t_errorf_v(t, "SignMessage error = %q, want %q", error_text(err), want);
    if (sig.len != 0 || fs.calls != 0)
        testing_t_errorf_v(t, "SignMessage signed anyway");
    arena_free(&ar);
}

static void TestSignMessageMessageSigner(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    MsgSigner ms = {0};
    CryptoSigner s = {&msg_signer_vt, &ms};
    CryptoHash h = CRYPTO_SHA256;
    IoReader rand = {0};
    Error err = BURROW_NO_ERROR;
    Slice sig = crypto_sign_message(a, s, rand, text("abc"),
                                    crypto_hash_as_signer_opts(&h), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "SignMessage: %v", err);
    if (ms.Calls != 1 || !bytes_equal(sig, text("abc")))
        testing_t_errorf_v(t, "SignMessage did not hand the message to SignMessage: %q",
                           sig);
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestRegisterHashLimits)                                                          \
    X(TestMLDSAMu)                                                                     \
    X(TestHashes)                                                                      \
    X(TestUnknownHash)                                                                 \
    X(TestRegisterHash)                                                                \
    X(TestSignMessageDigest)                                                           \
    X(TestSignMessageUnavailable)                                                      \
    X(TestSignMessageMessageSigner)

TESTING_MAIN(TESTS)
