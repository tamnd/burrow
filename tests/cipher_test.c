/* Derived from Go's src/crypto/cipher/benchmark_test.go, cbc_aes_test.go,
 * cbc_test.go, cfb_test.go, common_test.go, ctr_aes_test.go, ctr_test.go,
 * fuzz_test.go and ofb_test.go. gcm_test.go is tests/gcm_test.c.
 * Go source: go1.27.1.
 *
 * What is left out:
 *
 *   - TestCBCExtraMethods, TestCTRExtraMethods and all of modes_test.go. They
 *     ask reflection what methods a value has, or whether NewCTR and friends
 *     pick up an undocumented method on the block, and a C table has nothing
 *     to ask.
 *   - The Wycheproof CBC vectors, which wait for the Wycheproof files.
 *
 * The Go tests seed math/rand with fixed numbers. The same seeds go into the
 * generator from testhash.h here, so the bytes differ from Go's, but none of
 * these tests depends on what the bytes are, only on two paths agreeing.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"
#include "testcipher.h"

#include "burrow/burrow.h"
#include "burrow/bytes.h"
#include "burrow/clock.h"
#include "burrow/crypto/aes.h"
#include "burrow/crypto/cipher.h"
#include "burrow/crypto/des.h"
#include "burrow/crypto/rand.h"
#include "burrow/encoding/hex.h"
#include "burrow/io.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------ common values */

static const Byte common_input[] = {
    0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96, 0xe9, 0x3d, 0x7e, 0x11, 0x73,
    0x93, 0x17, 0x2a, 0xae, 0x2d, 0x8a, 0x57, 0x1e, 0x03, 0xac, 0x9c, 0x9e, 0xb7,
    0x6f, 0xac, 0x45, 0xaf, 0x8e, 0x51, 0x30, 0xc8, 0x1c, 0x46, 0xa3, 0x5c, 0xe4,
    0x11, 0xe5, 0xfb, 0xc1, 0x19, 0x1a, 0x0a, 0x52, 0xef, 0xf6, 0x9f, 0x24, 0x45,
    0xdf, 0x4f, 0x9b, 0x17, 0xad, 0x2b, 0x41, 0x7b, 0xe6, 0x6c, 0x37, 0x10,
};

static const Byte common_key128[] = {0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
                                     0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c};

static const Byte common_key192[] = {
    0x8e, 0x73, 0xb0, 0xf7, 0xda, 0x0e, 0x64, 0x52, 0xc8, 0x10, 0xf3, 0x2b,
    0x80, 0x90, 0x79, 0xe5, 0x62, 0xf8, 0xea, 0xd2, 0x52, 0x2c, 0x6b, 0x7b,
};

static const Byte common_key256[] = {
    0x60, 0x3d, 0xeb, 0x10, 0x15, 0xca, 0x71, 0xbe, 0x2b, 0x73, 0xae,
    0xf0, 0x85, 0x7d, 0x77, 0x81, 0x1f, 0x35, 0x2c, 0x07, 0x3b, 0x61,
    0x08, 0xd7, 0x2d, 0x98, 0x10, 0xa3, 0x09, 0x14, 0xdf, 0xf4,
};

static const Byte common_iv[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                                 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};

static const Byte common_counter[] = {0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7,
                                      0xf8, 0xf9, 0xfa, 0xfb, 0xfc, 0xfd, 0xfe, 0xff};

static Slice bs(const Byte *p, Int n) {
    return slice_from((void *)(uintptr_t)p, n, n, TYPE_BYTE);
}

static Slice clone(Alloc *a, const Byte *p, Int n) {
    Slice s = testhash_bytes(a, n);
    if (n > 0)
        memcpy(s.p, p, (size_t)n);
    return s;
}

static Slice unhex(Alloc *a, const char *s) {
    Error err = BURROW_NO_ERROR;
    Slice b = hex_decode_string(a, str_from_cstr(s), &err);
    if (BURROW_FAILED(err))
        panic_str(BURROW_S("bad hex in a test table"));
    return b;
}

static CipherBlock must_aes(Alloc *a, Slice key) {
    Error err = BURROW_NO_ERROR;
    CipherBlock c = aes_new_cipher(a, key, &err);
    if (BURROW_FAILED(err))
        panic(BURROW_ANY(TYPE_ERROR, &err));
    return c;
}

/* The AES block behind a table of its own, so the modes cannot see it is AES
 * and take their general paths. gcm_test.c has the same one. */
typedef struct Wrapper {
    CipherBlock block;
} Wrapper;

static Int wrapper_block_size(void *self) {
    return cipher_block_block_size(((Wrapper *)self)->block);
}

static void wrapper_encrypt(void *self, Slice dst, Slice src) {
    cipher_block_encrypt(((Wrapper *)self)->block, dst, src);
}

static void wrapper_decrypt(void *self, Slice dst, Slice src) {
    cipher_block_decrypt(((Wrapper *)self)->block, dst, src);
}

static const CipherBlockVT wrapper_vt = {NULL, wrapper_block_size, wrapper_encrypt,
                                         wrapper_decrypt};

static CipherBlock wrap(Alloc *a, CipherBlock b) {
    Wrapper *w = BURROW_NEW(a, Wrapper);
    w->block = b;
    CipherBlock out = {&wrapper_vt, w};
    return out;
}

/* A 128, 192 and 256 bit subtest for each, with a fresh random key. */
typedef void (*KeyTest)(TestingT *t, CipherBlock block);

typedef struct KeyEnv {
    KeyTest f;
    Int bits;
} KeyEnv;

static void key_size_one(void *env, TestingT *t) {
    const KeyEnv *e = env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TesthashRand rng = testhash_new_rand(t);
    Slice key = testhash_bytes(a, e->bits / 8);
    testhash_read(&rng, key);
    e->f(t, must_aes(a, key));
    arena_free(&ar);
}

static void each_key_size(TestingT *t, KeyTest f) {
    static const Int bits[] = {128, 192, 256};
    for (size_t i = 0; i < 3; i++) {
        KeyEnv e = {f, bits[i]};
        Str name = bits[i] == 128   ? BURROW_S("AES-128")
                   : bits[i] == 192 ? BURROW_S("AES-192")
                                    : BURROW_S("AES-256");
        testing_t_run(t, name, BURROW_FN(TestingTFunc, key_size_one, &e));
    }
}

static void each_key_size_env(void *env, TestingT *t) {
    each_key_size(t, *(const KeyTest *)env);
}

static void des_one(void *env, TestingT *t) {
    KeyTest f = *(const KeyTest *)env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TesthashRand rng = testhash_new_rand(t);
    Slice key = testhash_bytes(a, 8);
    testhash_read(&rng, key);
    Error err = BURROW_NO_ERROR;
    CipherBlock block = des_new_cipher(a, key, &err);
    if (BURROW_FAILED(err))
        panic(BURROW_ANY(TYPE_ERROR, &err));
    f(t, block);
    arena_free(&ar);
}

/* The same with DES and a fresh random key, as a "DES" subtest. */
static void des_subtest(TestingT *t, KeyTest f) {
    testing_t_run(t, BURROW_S("DES"), BURROW_FN(TestingTFunc, des_one, &f));
}

/* ---------------------------------------------------------------------- CBC */

typedef struct ModeTest {
    const char *name;
    const Byte *key;
    Int key_len;
    const Byte *iv;
    const Byte *in;
    Byte out[64];
} ModeTest;

/* NIST SP 800-38A pp 27-29 */
static const ModeTest cbc_aes_tests[] = {
    {
        "CBC-AES128",
        common_key128,
        16,
        common_iv,
        common_input,
        {
            0x76, 0x49, 0xab, 0xac, 0x81, 0x19, 0xb2, 0x46, 0xce, 0xe9, 0x8e,
            0x9b, 0x12, 0xe9, 0x19, 0x7d, 0x50, 0x86, 0xcb, 0x9b, 0x50, 0x72,
            0x19, 0xee, 0x95, 0xdb, 0x11, 0x3a, 0x91, 0x76, 0x78, 0xb2, 0x73,
            0xbe, 0xd6, 0xb8, 0xe3, 0xc1, 0x74, 0x3b, 0x71, 0x16, 0xe6, 0x9e,
            0x22, 0x22, 0x95, 0x16, 0x3f, 0xf1, 0xca, 0xa1, 0x68, 0x1f, 0xac,
            0x09, 0x12, 0x0e, 0xca, 0x30, 0x75, 0x86, 0xe1, 0xa7,
        },
    },
    {
        "CBC-AES192",
        common_key192,
        24,
        common_iv,
        common_input,
        {
            0x4f, 0x02, 0x1d, 0xb2, 0x43, 0xbc, 0x63, 0x3d, 0x71, 0x78, 0x18,
            0x3a, 0x9f, 0xa0, 0x71, 0xe8, 0xb4, 0xd9, 0xad, 0xa9, 0xad, 0x7d,
            0xed, 0xf4, 0xe5, 0xe7, 0x38, 0x76, 0x3f, 0x69, 0x14, 0x5a, 0x57,
            0x1b, 0x24, 0x20, 0x12, 0xfb, 0x7a, 0xe0, 0x7f, 0xa9, 0xba, 0xac,
            0x3d, 0xf1, 0x02, 0xe0, 0x08, 0xb0, 0xe2, 0x79, 0x88, 0x59, 0x88,
            0x81, 0xd9, 0x20, 0xa9, 0xe6, 0x4f, 0x56, 0x15, 0xcd,
        },
    },
    {
        "CBC-AES256",
        common_key256,
        32,
        common_iv,
        common_input,
        {
            0xf5, 0x8c, 0x4c, 0x04, 0xd6, 0xe5, 0xf1, 0xba, 0x77, 0x9e, 0xab,
            0xfb, 0x5f, 0x7b, 0xfb, 0xd6, 0x9c, 0xfc, 0x4e, 0x96, 0x7e, 0xdb,
            0x80, 0x8d, 0x67, 0x9f, 0x77, 0x7b, 0xc6, 0x70, 0x2c, 0x7d, 0x39,
            0xf2, 0x33, 0x69, 0xa9, 0xd9, 0xba, 0xcf, 0xa5, 0x30, 0xe2, 0x63,
            0x04, 0x23, 0x14, 0x61, 0xb2, 0xeb, 0x05, 0xe2, 0xc3, 0x9b, 0xe9,
            0xfc, 0xda, 0x6c, 0x19, 0x07, 0x8c, 0x6a, 0x9d, 0x1b,
        },
    },
};

static void test_cbc_encrypter_aes(void *env, TestingT *t) {
    (void)env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < 3; i++) {
        const ModeTest *test = &cbc_aes_tests[i];
        Error err = BURROW_NO_ERROR;
        CipherBlock c = aes_new_cipher(a, bs(test->key, test->key_len), &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "%s: NewCipher(%d bytes) = %v",
                               str_from_cstr(test->name), test->key_len, err);
            continue;
        }
        CipherBlockMode encrypter = cipher_new_cbc_encrypter(a, c, bs(test->iv, 16));
        Slice data = clone(a, test->in, 64);
        cipher_block_mode_crypt_blocks(encrypter, data, data);
        if (!testhash_equal(bs(test->out, 64), data))
            testing_t_errorf_v(t, "%s: CBCEncrypter\nhave %x\nwant %x",
                               str_from_cstr(test->name), data, bs(test->out, 64));
    }
    arena_free(&ar);
}

static void TestCBCEncrypterAES(TestingT *t) {
    testcipher_all_implementations(t, test_cbc_encrypter_aes, NULL);
}

static void test_cbc_decrypter_aes(void *env, TestingT *t) {
    (void)env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < 3; i++) {
        const ModeTest *test = &cbc_aes_tests[i];
        Error err = BURROW_NO_ERROR;
        CipherBlock c = aes_new_cipher(a, bs(test->key, test->key_len), &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "%s: NewCipher(%d bytes) = %v",
                               str_from_cstr(test->name), test->key_len, err);
            continue;
        }
        CipherBlockMode decrypter = cipher_new_cbc_decrypter(a, c, bs(test->iv, 16));
        Slice data = clone(a, test->out, 64);
        cipher_block_mode_crypt_blocks(decrypter, data, data);
        if (!testhash_equal(bs(test->in, 64), data))
            testing_t_errorf_v(t, "%s: CBCDecrypter\nhave %x\nwant %x",
                               str_from_cstr(test->name), data, bs(test->in, 64));
    }
    arena_free(&ar);
}

static void TestCBCDecrypterAES(TestingT *t) {
    testcipher_all_implementations(t, test_cbc_decrypter_aes, NULL);
}

static void cbc_block_mode(TestingT *t, CipherBlock block) {
    testcipher_block_mode(t, block, cipher_new_cbc_encrypter, cipher_new_cbc_decrypter);
}

/* Test CBC Blockmode against the general cipher.BlockMode interface tester. */
static void TestCBCBlockMode(TestingT *t) {
    static const KeyTest f = cbc_block_mode;
    testcipher_all_implementations(t, each_key_size_env, (void *)(uintptr_t)&f);
    des_subtest(t, cbc_block_mode);
}

/* Not Go's: SetIV, which Go only reaches through NoExtraMethods. Decrypting
 * twice from the same IV has to give the same plaintext both times. */
static void TestCBCSetIV(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    const ModeTest *test = &cbc_aes_tests[0];
    CipherBlock c = must_aes(a, bs(test->key, 16));
    CipherBlockMode d = cipher_new_cbc_decrypter(a, c, bs(common_iv, 16));
    for (int round = 0; round < 2; round++) {
        Slice data = clone(a, test->out, 64);
        cipher_block_mode_crypt_blocks(d, data, data);
        if (!testhash_equal(bs(test->in, 64), data))
            testing_t_errorf_v(t, "round %d: have %x\nwant %x", round, data,
                               bs(test->in, 64));
        cipher_cbc_set_iv(d, bs(common_iv, 16));
    }

    bool panicked = false;
    BURROW_TRY {
        cipher_cbc_set_iv(d, bs(common_iv, 15));
    }
    BURROW_CATCH(r) {
        (void)r;
        panicked = true;
    }
    BURROW_TRY_END;
    if (!panicked)
        testing_t_errorf_v(t, "SetIV with a 15 byte IV did not panic");
    arena_free(&ar);
}

/* Go's version hands CryptBlocks its buffers the other way round, input as
 * the destination, so it compares two buffers that nothing writes. This one
 * does what its messages say: random input through both, the outputs
 * compared. */
static void TestFuzz(TestingT *t) {
    enum { DATALEN = 1024 };
    static const struct {
        const char *name;
        const Byte *key;
        Int key_len;
    } tests[] = {
        {"CBC-AES128", common_key128, 16},
        {"CBC-AES192", common_key192, 24},
        {"CBC-AES256", common_key256, 32},
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    int64_t limit = testing_short() ? 10 * 1000 * 1000 : 2000 * 1000 * 1000;
    Slice indata = testhash_bytes(a, DATALEN);
    Slice outgeneric = testhash_bytes(a, DATALEN);
    Slice outdata = testhash_bytes(a, DATALEN);
    for (size_t i = 0; i < 3; i++) {
        CipherBlock c = must_aes(a, bs(tests[i].key, tests[i].key_len));
        for (int dir = 0; dir < 2; dir++) {
            CipherBlockMode cbc_asm, cbc_generic;
            if (dir == 0) {
                cbc_asm = cipher_new_cbc_encrypter(a, c, bs(common_iv, 16));
                cbc_generic =
                    cipher_new_cbc_encrypter(a, wrap(a, c), bs(common_iv, 16));
            } else {
                cbc_asm = cipher_new_cbc_decrypter(a, c, bs(common_iv, 16));
                cbc_generic =
                    cipher_new_cbc_decrypter(a, wrap(a, c), bs(common_iv, 16));
            }
            int64_t deadline = burrow__nanotime() + limit;
            while (burrow__nanotime() < deadline) {
                crypto_rand_read(indata, NULL);
                cipher_block_mode_crypt_blocks(cbc_generic, outgeneric, indata);
                cipher_block_mode_crypt_blocks(cbc_asm, outdata, indata);
                if (!testhash_equal(outdata, outgeneric))
                    testing_t_fatalf_v(
                        t, "AES-CBC %s does not match reference result: %x and %x",
                        dir == 0 ? BURROW_S("encryption") : BURROW_S("decryption"),
                        outdata, outgeneric);
            }
        }
    }
    arena_free(&ar);
}

/* ---------------------------------------------------------------------- CFB */

/* From NIST SP 800-38A, section F.3.13. */
static const struct {
    const char *key, *iv, *plaintext, *ciphertext;
} cfb_tests[] = {
    {
        "2b7e151628aed2a6abf7158809cf4f3c",
        "000102030405060708090a0b0c0d0e0f",
        "6bc1bee22e409f96e93d7e117393172a",
        "3b3fd92eb72dad20333449f8e83cfb4a",
    },
    {
        "2b7e151628aed2a6abf7158809cf4f3c",
        "3B3FD92EB72DAD20333449F8E83CFB4A",
        "ae2d8a571e03ac9c9eb76fac45af8e51",
        "c8a64537a0b3a93fcde3cdad9f1ce58b",
    },
    {
        "2b7e151628aed2a6abf7158809cf4f3c",
        "C8A64537A0B3A93FCDE3CDAD9F1CE58B",
        "30c81c46a35ce411e5fbc1191a0a52ef",
        "26751f67a3cbb140b1808cf187a4f4df",
    },
    {
        "2b7e151628aed2a6abf7158809cf4f3c",
        "26751F67A3CBB140B1808CF187A4F4DF",
        "f69f2445df4f9b17ad2b417be66c3710",
        "c04b05357c5d1c0eeac4c66f9ff7f2e6",
    },
};

static void TestCFBVectors(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof cfb_tests / sizeof cfb_tests[0]; i++) {
        Slice key = unhex(a, cfb_tests[i].key);
        Slice iv = unhex(a, cfb_tests[i].iv);
        Slice plaintext = unhex(a, cfb_tests[i].plaintext);
        Slice expected = unhex(a, cfb_tests[i].ciphertext);
        CipherBlock block = must_aes(a, key);

        Slice ciphertext = testhash_bytes(a, plaintext.len);
        CipherStream cfb = cipher_new_cfb_encrypter(a, block, iv);
        cipher_stream_xor_key_stream(cfb, ciphertext, plaintext);
        if (!testhash_equal(ciphertext, expected))
            testing_t_errorf_v(t, "#%d: wrong output: got %x, expected %x", (Int)i,
                               ciphertext, expected);

        CipherStream cfbdec = cipher_new_cfb_decrypter(a, block, iv);
        Slice plaintext_copy = testhash_bytes(a, ciphertext.len);
        cipher_stream_xor_key_stream(cfbdec, plaintext_copy, ciphertext);
        if (!testhash_equal(plaintext_copy, plaintext))
            testing_t_errorf_v(t, "#%d: wrong plaintext: got %x, expected %x", (Int)i,
                               plaintext_copy, plaintext);
    }
    arena_free(&ar);
}

static void TestCFBInverse(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    CipherBlock block = must_aes(a, bs(common_key128, 16));
    Slice plaintext =
        slice_from_str(a, BURROW_S("this is the plaintext. this is the plaintext."));
    Slice iv = testhash_bytes(a, cipher_block_block_size(block));
    crypto_rand_read(iv, NULL);
    CipherStream cfb = cipher_new_cfb_encrypter(a, block, iv);
    Slice ciphertext = testhash_clone(a, plaintext);
    cipher_stream_xor_key_stream(cfb, ciphertext, ciphertext);

    CipherStream cfbdec = cipher_new_cfb_decrypter(a, block, iv);
    Slice plaintext_copy = testhash_clone(a, ciphertext);
    cipher_stream_xor_key_stream(cfbdec, plaintext_copy, plaintext_copy);
    if (!testhash_equal(plaintext_copy, plaintext))
        testing_t_errorf_v(t, "got: %x, want: %x", plaintext_copy, plaintext);
    arena_free(&ar);
}

typedef struct StreamFromBlockEnv {
    CipherBlock block;
    TestcipherStreamMode mode;
} StreamFromBlockEnv;

static void stream_from_block(void *env, TestingT *t) {
    const StreamFromBlockEnv *e = env;
    testcipher_stream_from_block(t, e->block, e->mode);
}

static void cfb_stream(TestingT *t, CipherBlock block) {
    StreamFromBlockEnv enc = {block, cipher_new_cfb_encrypter};
    StreamFromBlockEnv dec = {block, cipher_new_cfb_decrypter};
    testing_t_run(t, BURROW_S("Encrypter"),
                  BURROW_FN(TestingTFunc, stream_from_block, &enc));
    testing_t_run(t, BURROW_S("Decrypter"),
                  BURROW_FN(TestingTFunc, stream_from_block, &dec));
}

static void TestCFBStream(TestingT *t) {
    each_key_size(t, cfb_stream);
    des_subtest(t, cfb_stream);
}

/* ---------------------------------------------------------------------- CTR */

/* A block whose encryption is the identity, so CTR's key stream is the
 * counter itself. */
static Int noop_block_size(void *self) {
    return *(const Int *)self;
}

static void noop_encrypt(void *self, Slice dst, Slice src) {
    (void)self;
    memmove(dst.p, src.p, (size_t)(dst.len < src.len ? dst.len : src.len));
}

static void noop_decrypt(void *self, Slice dst, Slice src) {
    (void)self;
    (void)dst;
    (void)src;
    panic_str(BURROW_S("unreachable"));
}

static const CipherBlockVT noop_block_vt = {NULL, noop_block_size, noop_encrypt,
                                            noop_decrypt};

static void inc(Slice b) {
    Byte *p = (Byte *)b.p;
    for (Int i = b.len - 1; i >= 0; i--) {
        p[i]++;
        if (p[i] != 0)
            break;
    }
}

static void xor_bytes(Byte *a, const Byte *b, Int n) {
    for (Int i = 0; i < n; i++)
        a[i] ^= b[i];
}

static void TestCTR(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int size = 64; size <= 1024; size *= 2) {
        Slice iv = testhash_bytes(a, size);
        CipherBlock noop = {&noop_block_vt, &size};
        CipherStream ctr = cipher_new_ctr(a, noop, iv);
        Slice src = testhash_bytes(a, 1024);
        memset(src.p, 0xff, 1024);
        Slice want = testhash_clone(a, src);
        Slice counter = testhash_bytes(a, size);
        for (Int i = 1; i < want.len / size; i++) {
            inc(counter);
            xor_bytes((Byte *)want.p + i * size, (const Byte *)counter.p, size);
        }
        Slice dst = testhash_bytes(a, 1024);
        cipher_stream_xor_key_stream(ctr, dst, src);
        if (!testhash_equal(dst, want))
            testing_t_errorf_v(t, "for size %d\nhave %x\nwant %x", size, dst, want);
    }
    arena_free(&ar);
}

static void ctr_stream(TestingT *t, CipherBlock block) {
    testcipher_stream_from_block(t, block, cipher_new_ctr);
}

static void TestCTRStream(TestingT *t) {
    static const KeyTest f = ctr_stream;
    testcipher_all_implementations(t, each_key_size_env, (void *)(uintptr_t)&f);
    des_subtest(t, ctr_stream);
}

/* NIST SP 800-38A pp 55-58 */
static const ModeTest ctr_aes_tests[] = {
    {
        "CTR-AES128",
        common_key128,
        16,
        common_counter,
        common_input,
        {
            0x87, 0x4d, 0x61, 0x91, 0xb6, 0x20, 0xe3, 0x26, 0x1b, 0xef, 0x68,
            0x64, 0x99, 0x0d, 0xb6, 0xce, 0x98, 0x06, 0xf6, 0x6b, 0x79, 0x70,
            0xfd, 0xff, 0x86, 0x17, 0x18, 0x7b, 0xb9, 0xff, 0xfd, 0xff, 0x5a,
            0xe4, 0xdf, 0x3e, 0xdb, 0xd5, 0xd3, 0x5e, 0x5b, 0x4f, 0x09, 0x02,
            0x0d, 0xb0, 0x3e, 0xab, 0x1e, 0x03, 0x1d, 0xda, 0x2f, 0xbe, 0x03,
            0xd1, 0x79, 0x21, 0x70, 0xa0, 0xf3, 0x00, 0x9c, 0xee,
        },
    },
    {
        "CTR-AES192",
        common_key192,
        24,
        common_counter,
        common_input,
        {
            0x1a, 0xbc, 0x93, 0x24, 0x17, 0x52, 0x1c, 0xa2, 0x4f, 0x2b, 0x04,
            0x59, 0xfe, 0x7e, 0x6e, 0x0b, 0x09, 0x03, 0x39, 0xec, 0x0a, 0xa6,
            0xfa, 0xef, 0xd5, 0xcc, 0xc2, 0xc6, 0xf4, 0xce, 0x8e, 0x94, 0x1e,
            0x36, 0xb2, 0x6b, 0xd1, 0xeb, 0xc6, 0x70, 0xd1, 0xbd, 0x1d, 0x66,
            0x56, 0x20, 0xab, 0xf7, 0x4f, 0x78, 0xa7, 0xf6, 0xd2, 0x98, 0x09,
            0x58, 0x5a, 0x97, 0xda, 0xec, 0x58, 0xc6, 0xb0, 0x50,
        },
    },
    {
        "CTR-AES256",
        common_key256,
        32,
        common_counter,
        common_input,
        {
            0x60, 0x1e, 0xc3, 0x13, 0x77, 0x57, 0x89, 0xa5, 0xb7, 0xa7, 0xf5,
            0x04, 0xbb, 0xf3, 0xd2, 0x28, 0xf4, 0x43, 0xe3, 0xca, 0x4d, 0x62,
            0xb5, 0x9a, 0xca, 0x84, 0xe9, 0x90, 0xca, 0xca, 0xf5, 0xc5, 0x2b,
            0x09, 0x30, 0xda, 0xa2, 0x3d, 0xe9, 0x4c, 0xe8, 0x70, 0x17, 0xba,
            0x2d, 0x84, 0x98, 0x8d, 0xdf, 0xc9, 0xc5, 0x8d, 0xb6, 0x7a, 0xad,
            0xa6, 0x13, 0xc2, 0xdd, 0x08, 0x45, 0x79, 0x41, 0xa6,
        },
    },
};

static void test_ctr_aes(void *env, TestingT *t) {
    (void)env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < 3; i++) {
        const ModeTest *tt = &ctr_aes_tests[i];
        Str test = str_from_cstr(tt->name);
        Error err = BURROW_NO_ERROR;
        CipherBlock c = aes_new_cipher(a, bs(tt->key, tt->key_len), &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "%s: NewCipher(%d bytes) = %v", test, tt->key_len,
                               err);
            continue;
        }

        for (Int j = 0; j <= 5; j += 5) {
            Slice in = bs(tt->in, 64 - j);
            CipherStream ctr = cipher_new_ctr(a, c, bs(tt->iv, 16));
            Slice encrypted = testhash_bytes(a, in.len);
            cipher_stream_xor_key_stream(ctr, encrypted, in);
            Slice out = bs(tt->out, in.len);
            if (!testhash_equal(out, encrypted))
                testing_t_errorf_v(t, "%s/%d: CTR\ninpt %x\nhave %x\nwant %x", test,
                                   in.len, in, encrypted, out);
        }

        for (Int j = 0; j <= 7; j += 7) {
            Slice in = bs(tt->out, 64 - j);
            CipherStream ctr = cipher_new_ctr(a, c, bs(tt->iv, 16));
            Slice plain = testhash_bytes(a, in.len);
            cipher_stream_xor_key_stream(ctr, plain, in);
            Slice out = bs(tt->in, in.len);
            if (!testhash_equal(out, plain))
                testing_t_errorf_v(t, "%s/%d: CTRReader\nhave %x\nwant %x", test,
                                   out.len, plain, out);
        }

        if (testing_t_failed(t))
            break;
    }
    arena_free(&ar);
}

static void TestCTR_AES(TestingT *t) {
    testcipher_all_implementations(t, test_ctr_aes, NULL);
}

static void make_testing_ciphers(Alloc *a, CipherBlock aes_block, Slice iv,
                                 CipherStream *generic_ctr,
                                 CipherStream *multiblock_ctr) {
    *generic_ctr = cipher_new_ctr(a, wrap(a, aes_block), iv);
    *multiblock_ctr = cipher_new_ctr(a, aes_block, iv);
}

static void put_be64(Byte *p, uint64_t v) {
    for (int i = 7; i >= 0; i--) {
        p[i] = (Byte)v;
        v >>= 8;
    }
}

static Slice extract_counters(Alloc *a, CipherBlock block, Slice keystream) {
    Int block_size = cipher_block_block_size(block);
    Slice res = testhash_bytes(a, keystream.len);
    for (Int i = 0; i < keystream.len; i += block_size)
        cipher_block_decrypt(block, slice_sub(res, i, i + block_size),
                             slice_sub(keystream, i, i + block_size));
    return res;
}

typedef struct Blocks8Case {
    const char *name;
    uint64_t hi, lo;
    CipherBlock block;
} Blocks8Case;

static void blocks8_case(void *env, TestingT *t) {
    const Blocks8Case *tc = env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice keystream = testhash_bytes(a, 8 * 16);
    Byte iv[16];
    put_be64(iv, tc->hi);
    put_be64(iv + 8, tc->lo);
    CipherStream generic, multiblock;
    make_testing_ciphers(a, tc->block, bs(iv, 16), &generic, &multiblock);

    Slice generic_out = testhash_bytes(a, keystream.len);
    Slice multiblock_out = testhash_bytes(a, keystream.len);
    cipher_stream_xor_key_stream(generic, generic_out, keystream);
    cipher_stream_xor_key_stream(multiblock, multiblock_out, keystream);
    if (!testhash_equal(multiblock_out, generic_out))
        testing_t_fatalf_v(t,
                           "mismatch for iv %#x:%#x\nasm keystream: %x\ngen keystream: "
                           "%x\nasm counters: %x\ngen counters: %x",
                           tc->hi, tc->lo, multiblock_out, generic_out,
                           extract_counters(a, tc->block, multiblock_out),
                           extract_counters(a, tc->block, generic_out));
    arena_free(&ar);
}

/* That the eight block fast path walks the counter the way the generic one
 * does, near overflow too. */
static void TestCTR_AES_blocks8FastPathMatchesGeneric(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice key = testhash_bytes(a, 16);
    CipherBlock block = must_aes(a, key);
    if (burrow__aes_block_of(block) == NULL)
        testing_t_skip_v(t, "requires crypto/internal/fips140/aes");

    Blocks8Case cases[] = {
        {"Zero", 0, 0, block},
        {"NearOverflowMinus7", 1, ~(uint64_t)0 - 7, block},
        {"NearOverflowMinus6", 2, ~(uint64_t)0 - 6, block},
        {"Overflow", 0, ~(uint64_t)0, block},
    };
    for (size_t i = 0; i < 4; i++)
        testing_t_run(t, str_from_cstr(cases[i].name),
                      BURROW_FN(TestingTFunc, blocks8_case, &cases[i]));
    arena_free(&ar);
}

static TesthashRand seeded(uint64_t seed) {
    TesthashRand r = {seed};
    return r;
}

static Slice rand_bytes(Alloc *a, TesthashRand *r, Int count) {
    Slice buf = testhash_bytes(a, count);
    testhash_read(r, buf);
    return buf;
}

enum { RANDOM_IV_SIZE = 100 };

typedef struct RandomIVEnv {
    TesthashRand *r;
    Slice iv;
    Int key_size;
    CipherBlock aes_block;
    Slice plaintext;
    Slice generic_ciphertext;
    Int part1, part2;
} RandomIVEnv;

static void random_iv_part2(void *env, TestingT *t) {
    const RandomIVEnv *e = env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    CipherStream generic, multiblock_ctr;
    make_testing_ciphers(a, e->aes_block, e->iv, &generic, &multiblock_ctr);
    (void)generic;
    Int p1 = e->part1, p2 = e->part1 + e->part2;
    Slice c = testhash_bytes(a, e->plaintext.len);
    cipher_stream_xor_key_stream(multiblock_ctr, slice_sub(c, 0, p1),
                                 slice_sub(e->plaintext, 0, p1));
    cipher_stream_xor_key_stream(multiblock_ctr, slice_sub(c, p1, p2),
                                 slice_sub(e->plaintext, p1, p2));
    cipher_stream_xor_key_stream(multiblock_ctr, slice_sub(c, p2, c.len),
                                 slice_sub(e->plaintext, p2, e->plaintext.len));
    if (!testhash_equal(e->generic_ciphertext, c))
        testing_t_fatalf_v(
            t, "multiblock CTR's output does not match generic CTR's output");
    arena_free(&ar);
}

static void random_iv_part1(void *env, TestingT *t) {
    RandomIVEnv e = *(const RandomIVEnv *)env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (e.part2 = 0; e.part2 <= RANDOM_IV_SIZE - e.part1; e.part2++) {
        ArenaMark m = arena_mark(&ar);
        testing_t_run(t, fmt_sprintf_v(a, "part2=%d", e.part2),
                      BURROW_FN(TestingTFunc, random_iv_part2, &e));
        arena_release(&ar, m);
    }
    arena_free(&ar);
}

static void random_iv_key_size(void *env, TestingT *t) {
    RandomIVEnv e = *(const RandomIVEnv *)env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice key = rand_bytes(a, e.r, e.key_size);
    e.aes_block = must_aes(a, key);
    CipherStream generic_ctr, multiblock;
    make_testing_ciphers(a, e.aes_block, e.iv, &generic_ctr, &multiblock);
    (void)multiblock;
    e.plaintext = rand_bytes(a, e.r, RANDOM_IV_SIZE);

    /* Generate reference ciphertext. */
    e.generic_ciphertext = testhash_bytes(a, e.plaintext.len);
    cipher_stream_xor_key_stream(generic_ctr, e.generic_ciphertext, e.plaintext);

    /* Split the text in 3 parts in all possible ways and encrypt them
     * individually using multiblock implementation to catch edge cases. */
    for (e.part1 = 0; e.part1 <= RANDOM_IV_SIZE; e.part1++) {
        ArenaMark m = arena_mark(&ar);
        testing_t_run(t, fmt_sprintf_v(a, "part1=%d", e.part1),
                      BURROW_FN(TestingTFunc, random_iv_part1, &e));
        arena_release(&ar, m);
    }
    arena_free(&ar);
}

/* That multiblock AES CTR gives what the generic one block at a time code
 * gives, from a random IV. */
static void TestCTR_AES_multiblock_random_IV(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TesthashRand r = seeded(54321);
    RandomIVEnv e = {0};
    e.r = &r;
    e.iv = rand_bytes(a, &r, 16);
    static const Int key_sizes[] = {16, 24, 32};
    for (size_t i = 0; i < 3; i++) {
        e.key_size = key_sizes[i];
        testing_t_run(t, fmt_sprintf_v(a, "keySize=%d", key_sizes[i]),
                      BURROW_FN(TestingTFunc, random_iv_key_size, &e));
    }
    arena_free(&ar);
}

enum { OVERFLOW_SIZE = 4096 };

typedef struct OverflowEnv {
    TesthashRand *r;
    Slice plaintext;
    Int key_size;
    CipherBlock aes_block;
    Slice iv;
    Int offset;
} OverflowEnv;

static void overflow_offset(void *env, TestingT *t) {
    const OverflowEnv *e = env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    CipherStream generic_ctr, multiblock_ctr;
    make_testing_ciphers(a, e->aes_block, e->iv, &generic_ctr, &multiblock_ctr);

    /* Generate reference ciphertext. */
    Slice generic_ciphertext = testhash_bytes(a, OVERFLOW_SIZE);
    cipher_stream_xor_key_stream(generic_ctr, generic_ciphertext, e->plaintext);

    Slice multiblock_ciphertext = testhash_bytes(a, OVERFLOW_SIZE);
    cipher_stream_xor_key_stream(multiblock_ctr, multiblock_ciphertext,
                                 slice_sub(e->plaintext, 0, e->offset));
    cipher_stream_xor_key_stream(
        multiblock_ctr, slice_sub(multiblock_ciphertext, e->offset, OVERFLOW_SIZE),
        slice_sub(e->plaintext, e->offset, OVERFLOW_SIZE));
    if (!testhash_equal(generic_ciphertext, multiblock_ciphertext))
        testing_t_fatalf_v(
            t, "multiblock CTR's output does not match generic CTR's output");
    arena_free(&ar);
}

static void overflow_iv(void *env, TestingT *t) {
    OverflowEnv e = *(const OverflowEnv *)env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    static const Int offsets[] = {0, 1, 16, 1024};
    for (size_t i = 0; i < 4; i++) {
        e.offset = offsets[i];
        testing_t_run(t, fmt_sprintf_v(a, "offset=%d", offsets[i]),
                      BURROW_FN(TestingTFunc, overflow_offset, &e));
    }
    arena_free(&ar);
}

static void overflow_key_size(void *env, TestingT *t) {
    static const char *const ivs[] = {
        "0000000000000000FFFFFFFFFFFFFFFF", "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF",
        "FFFFFFFFFFFFFFFF0000000000000000", "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFfe",
        "0000000000000000FFFFFFFFFFFFFFfe", "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFF00",
        "0000000000000001FFFFFFFFFFFFFF00", "0000000000000001FFFFFFFFFFFFFFFF",
        "0000000000000001FFFFFFFFFFFFFFfe", "0000000000000001FFFFFFFFFFFFFF00",
    };
    OverflowEnv e = *(const OverflowEnv *)env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof ivs / sizeof ivs[0]; i++) {
        e.iv = unhex(a, ivs[i]);
        Slice key = rand_bytes(a, e.r, e.key_size);
        e.aes_block = must_aes(a, key);
        testing_t_run(t, fmt_sprintf_v(a, "iv=%s", hex_encode_to_string(a, e.iv)),
                      BURROW_FN(TestingTFunc, overflow_iv, &e));
    }
    arena_free(&ar);
}

/* The same, on the IVs where the counter's halves carry into each other. */
static void TestCTR_AES_multiblock_overflow_IV(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TesthashRand r = seeded(987654);
    OverflowEnv e = {0};
    e.r = &r;
    e.plaintext = rand_bytes(a, &r, OVERFLOW_SIZE);
    static const Int key_sizes[] = {16, 24, 32};
    for (size_t i = 0; i < 3; i++) {
        e.key_size = key_sizes[i];
        testing_t_run(t, fmt_sprintf_v(a, "keySize=%d", key_sizes[i]),
                      BURROW_FN(TestingTFunc, overflow_key_size, &e));
    }
    arena_free(&ar);
}

typedef struct AtEnv {
    TesthashRand *r;
    Slice plaintext;
    Int key_size;
} AtEnv;

static int cmp_int(const void *x, const void *y) {
    Int a = *(const Int *)x, b = *(const Int *)y;
    return (a > b) - (a < b);
}

enum { AT_SIZE = 32 * 1024 * 1024, AT_N = 1000 };

static void at_key_size(void *env, TestingT *t) {
    const AtEnv *e = env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice key = rand_bytes(a, e->r, e->key_size);
    Slice iv = rand_bytes(a, e->r, 16);
    CipherBlock aes_block = must_aes(a, key);
    CipherStream generic_ctr, ctr_at;
    make_testing_ciphers(a, aes_block, iv, &generic_ctr, &ctr_at);

    /* Generate reference ciphertext. */
    Slice generic_ciphertext = testhash_bytes(a, AT_SIZE);
    cipher_stream_xor_key_stream(generic_ctr, generic_ciphertext, e->plaintext);

    /* Split the range to random slices. */
    Int *boundaries = BURROW_NEW_N(a, Int, AT_N + 2);
    Int *perm = BURROW_NEW_N(a, Int, AT_N + 1);
    Byte word[8];
    for (Int i = 0; i < AT_N; i++) {
        testhash_read(e->r, bs(word, 8));
        uint64_t v = 0;
        memcpy(&v, word, 8);
        boundaries[i] = (Int)(v % AT_SIZE);
    }
    boundaries[AT_N] = 0;
    boundaries[AT_N + 1] = AT_SIZE;
    qsort(boundaries, AT_N + 2, sizeof(Int), cmp_int);
    for (Int i = 0; i <= AT_N; i++)
        perm[i] = i;
    for (Int i = AT_N; i > 0; i--) {
        testhash_read(e->r, bs(word, 8));
        uint64_t v = 0;
        memcpy(&v, word, 8);
        Int j = (Int)(v % (uint64_t)(i + 1));
        Int tmp = perm[i];
        perm[i] = perm[j];
        perm[j] = tmp;
    }

    Slice multiblock_ciphertext = testhash_bytes(a, AT_SIZE);
    for (Int k = 0; k <= AT_N; k++) {
        Int begin = boundaries[perm[k]];
        Int end = boundaries[perm[k] + 1];
        (void)burrow__aes_ctr_xor_key_stream_at(
            ctr_at, slice_sub(multiblock_ciphertext, begin, end),
            slice_sub(e->plaintext, begin, end), (uint64_t)begin);
    }
    if (!testhash_equal(generic_ciphertext, multiblock_ciphertext))
        testing_t_fatalf_v(
            t, "multiblock CTR's output does not match generic CTR's output");
    arena_free(&ar);
}

/* That XORKeyStreamAt works from any offset, in any order. */
static void TestCTR_AES_multiblock_XORKeyStreamAt(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TesthashRand r = seeded(12345);
    AtEnv e = {&r, rand_bytes(a, &r, AT_SIZE), 0};
    static const Int key_sizes[] = {16, 24, 32};
    for (size_t i = 0; i < 3; i++) {
        e.key_size = key_sizes[i];
        testing_t_run(t, fmt_sprintf_v(a, "keySize=%d", key_sizes[i]),
                      BURROW_FN(TestingTFunc, at_key_size, &e));
    }
    arena_free(&ar);
}

/* Not Go's: XORKeyStreamAt is only there for CTR over AES. */
static void TestCTRXORKeyStreamAtNotAES(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    CipherBlock block = must_aes(a, bs(common_key128, 16));
    CipherStream generic = cipher_new_ctr(a, wrap(a, block), bs(common_iv, 16));
    Byte buf[16] = {0};
    if (burrow__aes_ctr_xor_key_stream_at(generic, bs(buf, 16), bs(buf, 16), 0))
        testing_t_errorf_v(t,
                           "XORKeyStreamAt worked on CTR over a block that is not AES");
    arena_free(&ar);
}

/* ---------------------------------------------------------------------- OFB */

/* NIST SP 800-38A pp 52-55 */
static const ModeTest ofb_tests[] = {
    {
        "OFB-AES128",
        common_key128,
        16,
        common_iv,
        common_input,
        {
            0x3b, 0x3f, 0xd9, 0x2e, 0xb7, 0x2d, 0xad, 0x20, 0x33, 0x34, 0x49,
            0xf8, 0xe8, 0x3c, 0xfb, 0x4a, 0x77, 0x89, 0x50, 0x8d, 0x16, 0x91,
            0x8f, 0x03, 0xf5, 0x3c, 0x52, 0xda, 0xc5, 0x4e, 0xd8, 0x25, 0x97,
            0x40, 0x05, 0x1e, 0x9c, 0x5f, 0xec, 0xf6, 0x43, 0x44, 0xf7, 0xa8,
            0x22, 0x60, 0xed, 0xcc, 0x30, 0x4c, 0x65, 0x28, 0xf6, 0x59, 0xc7,
            0x78, 0x66, 0xa5, 0x10, 0xd9, 0xc1, 0xd6, 0xae, 0x5e,
        },
    },
    {
        "OFB-AES192",
        common_key192,
        24,
        common_iv,
        common_input,
        {
            0xcd, 0xc8, 0x0d, 0x6f, 0xdd, 0xf1, 0x8c, 0xab, 0x34, 0xc2, 0x59,
            0x09, 0xc9, 0x9a, 0x41, 0x74, 0xfc, 0xc2, 0x8b, 0x8d, 0x4c, 0x63,
            0x83, 0x7c, 0x09, 0xe8, 0x17, 0x00, 0xc1, 0x10, 0x04, 0x01, 0x8d,
            0x9a, 0x9a, 0xea, 0xc0, 0xf6, 0x59, 0x6f, 0x55, 0x9c, 0x6d, 0x4d,
            0xaf, 0x59, 0xa5, 0xf2, 0x6d, 0x9f, 0x20, 0x08, 0x57, 0xca, 0x6c,
            0x3e, 0x9c, 0xac, 0x52, 0x4b, 0xd9, 0xac, 0xc9, 0x2a,
        },
    },
    {
        "OFB-AES256",
        common_key256,
        32,
        common_iv,
        common_input,
        {
            0xdc, 0x7e, 0x84, 0xbf, 0xda, 0x79, 0x16, 0x4b, 0x7e, 0xcd, 0x84,
            0x86, 0x98, 0x5d, 0x38, 0x60, 0x4f, 0xeb, 0xdc, 0x67, 0x40, 0xd2,
            0x0b, 0x3a, 0xc8, 0x8f, 0x6a, 0xd8, 0x2a, 0x4f, 0xb0, 0x8d, 0x71,
            0xab, 0x47, 0xa0, 0x86, 0xe8, 0x6e, 0xed, 0xf3, 0x9d, 0x1c, 0x5b,
            0xba, 0x97, 0xc4, 0x08, 0x01, 0x26, 0x14, 0x1d, 0x67, 0xf3, 0x7b,
            0xe8, 0x53, 0x8f, 0x5a, 0x8b, 0xe7, 0x40, 0xe4, 0x84,
        },
    },
};

static void TestOFB(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < 3; i++) {
        const ModeTest *tt = &ofb_tests[i];
        Str test = str_from_cstr(tt->name);
        Error err = BURROW_NO_ERROR;
        CipherBlock c = aes_new_cipher(a, bs(tt->key, tt->key_len), &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "%s: NewCipher(%d bytes) = %v", test, tt->key_len,
                               err);
            continue;
        }

        for (Int j = 0; j <= 5; j += 5) {
            Slice plaintext = bs(tt->in, 64 - j);
            CipherStream ofb = cipher_new_ofb(a, c, bs(tt->iv, 16));
            Slice ciphertext = testhash_bytes(a, plaintext.len);
            cipher_stream_xor_key_stream(ofb, ciphertext, plaintext);
            if (!testhash_equal(ciphertext, bs(tt->out, plaintext.len)))
                testing_t_errorf_v(
                    t, "%s/%d: encrypting\ninput % x\nhave % x\nwant % x", test,
                    plaintext.len, plaintext, ciphertext, bs(tt->out, 64));
        }

        for (Int j = 0; j <= 5; j += 5) {
            Slice ciphertext = bs(tt->out, 64 - j);
            CipherStream ofb = cipher_new_ofb(a, c, bs(tt->iv, 16));
            Slice plaintext = testhash_bytes(a, ciphertext.len);
            cipher_stream_xor_key_stream(ofb, plaintext, ciphertext);
            if (!testhash_equal(plaintext, bs(tt->in, ciphertext.len)))
                testing_t_errorf_v(t, "%s/%d: decrypting\nhave % x\nwant % x", test,
                                   ciphertext.len, plaintext, bs(tt->in, 64));
        }

        if (testing_t_failed(t))
            break;
    }
    arena_free(&ar);
}

static void ofb_stream(TestingT *t, CipherBlock block) {
    testcipher_stream_from_block(t, block, cipher_new_ofb);
}

static void TestOFBStream(TestingT *t) {
    each_key_size(t, ofb_stream);
    des_subtest(t, ofb_stream);
}

/* ------------------------------------------------------------------ streams */

/* Not Go's: ExampleStreamReader and ExampleStreamWriter as a test, through
 * io.Copy both ways, with the outputs the examples print. */
static void TestStreamReaderWriter(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice key = unhex(a, "6368616e676520746869732070617373");
    CipherBlock block = must_aes(a, key);
    Byte iv[16] = {0};

    BytesReader plain;
    bytes_reader_reset(&plain, slice_from_str(a, BURROW_S("some secret text")));
    BytesBuffer out = BYTES_BUFFER(a);
    CipherStreamWriter writer = {cipher_new_ofb(a, block, bs(iv, 16)),
                                 bytes_buffer_as_io_writer(&out),
                                 BURROW_NO_ERROR,
                                 {NULL, NULL}};
    Error err = BURROW_NO_ERROR;
    (void)io_copy(a, cipher_stream_writer_as_io_writer(&writer),
                  bytes_reader_as_io_reader(&plain), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "io.Copy to the StreamWriter: %v", err);
    Str got = hex_encode_to_string(a, bytes_buffer_bytes(&out));
    if (!str_eq(got, BURROW_S("cf0495cc6f75dafc23948538e79904a9")))
        testing_t_errorf_v(t, "StreamWriter wrote %s", got);
    if (BURROW_FAILED(cipher_stream_writer_close(writer)))
        testing_t_errorf_v(t, "Close with no closer failed");

    BytesReader encrypted;
    bytes_reader_reset(&encrypted, unhex(a, "cf0495cc6f75dafc23948538e79904a9"));
    CipherStreamReader reader = {cipher_new_ofb(a, block, bs(iv, 16)),
                                 bytes_reader_as_io_reader(&encrypted)};
    BytesBuffer back = BYTES_BUFFER(a);
    err = BURROW_NO_ERROR;
    (void)io_copy(a, bytes_buffer_as_io_writer(&back),
                  cipher_stream_reader_as_io_reader(&reader), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "io.Copy from the StreamReader: %v", err);
    if (!testhash_equal(bytes_buffer_bytes(&back),
                        slice_from_str(a, BURROW_S("some secret text"))))
        testing_t_errorf_v(t, "StreamReader read %q", bytes_buffer_bytes(&back));
    arena_free(&ar);
}

/* --------------------------------------------------------------- benchmarks */

typedef struct StreamBench {
    TestcipherStreamMode mode;
    Int len;
    Int key_size;
} StreamBench;

static void benchmark_aes_stream(void *env, TestingB *b) {
    const StreamBench *s = env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice buf = testhash_bytes(a, s->len);
    testing_b_set_bytes(b, (int64_t)buf.len);
    Slice key = testhash_bytes(a, s->key_size);
    Byte iv[16] = {0};
    CipherStream stream = s->mode(a, must_aes(a, key), bs(iv, 16));

    testing_b_reset_timer(b);
    for (Int i = 0; i < testing_b_n(b); i++)
        cipher_stream_xor_key_stream(stream, buf, buf);
    arena_free(&ar);
}

/* If we test exactly 1K blocks, we would generate exact multiples of the
 * cipher's block size, and the cipher stream fragments would always be word
 * size aligned, whereas non-aligned is a more typical use case. */
enum { ALMOST_1K = 1024 - 5, ALMOST_8K = 8 * 1024 - 5 };

typedef struct CtrBenchEnv {
    Int key_size;
    StreamBench cases[3];
} CtrBenchEnv;

static void bench_ctr_key(void *env, TestingB *b) {
    CtrBenchEnv *e = env;
    static const char *const names[] = {"50", "1K", "8K"};
    static const Int lens[] = {50, ALMOST_1K, ALMOST_8K};
    for (int i = 0; i < 3; i++) {
        StreamBench sb = {cipher_new_ctr, lens[i], e->key_size};
        e->cases[i] = sb;
        testing_b_run(b, str_from_cstr(names[i]),
                      BURROW_FN(TestingBFunc, benchmark_aes_stream, &e->cases[i]));
    }
}

static void BenchmarkAESCTR(TestingB *b) {
    static const Int key_bits[] = {128, 192, 256};
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (int i = 0; i < 3; i++) {
        CtrBenchEnv e = {key_bits[i] / 8, {{0}}};
        testing_b_run(b, fmt_sprintf_v(a, "%d", key_bits[i]),
                      BURROW_FN(TestingBFunc, bench_ctr_key, &e));
    }
    arena_free(&ar);
}

static void benchmark_cbc_1k(TestingB *b, bool encrypt) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice buf = testhash_bytes(a, 1024);
    testing_b_set_bytes(b, (int64_t)buf.len);
    Byte key[16] = {0};
    Byte iv[16] = {0};
    CipherBlock aes = must_aes(a, bs(key, 16));
    CipherBlockMode cbc = encrypt ? cipher_new_cbc_encrypter(a, aes, bs(iv, 16))
                                  : cipher_new_cbc_decrypter(a, aes, bs(iv, 16));
    for (Int i = 0; i < testing_b_n(b); i++)
        cipher_block_mode_crypt_blocks(cbc, buf, buf);
    arena_free(&ar);
}

static void BenchmarkAESCBCEncrypt1K(TestingB *b) {
    benchmark_cbc_1k(b, true);
}

static void BenchmarkAESCBCDecrypt1K(TestingB *b) {
    benchmark_cbc_1k(b, false);
}

#define TESTS(X)                                                                       \
    X(TestCBCEncrypterAES)                                                             \
    X(TestCBCDecrypterAES)                                                             \
    X(TestCBCBlockMode)                                                                \
    X(TestCBCSetIV)                                                                    \
    X(TestFuzz)                                                                        \
    X(TestCFBVectors)                                                                  \
    X(TestCFBInverse)                                                                  \
    X(TestCFBStream)                                                                   \
    X(TestCTR)                                                                         \
    X(TestCTRStream)                                                                   \
    X(TestCTR_AES)                                                                     \
    X(TestCTR_AES_blocks8FastPathMatchesGeneric)                                       \
    X(TestCTR_AES_multiblock_random_IV)                                                \
    X(TestCTR_AES_multiblock_overflow_IV)                                              \
    X(TestCTR_AES_multiblock_XORKeyStreamAt)                                           \
    X(TestCTRXORKeyStreamAtNotAES)                                                     \
    X(TestOFB)                                                                         \
    X(TestOFBStream)                                                                   \
    X(TestStreamReaderWriter)                                                          \
    X(BenchmarkAESCTR)                                                                 \
    X(BenchmarkAESCBCEncrypt1K)                                                        \
    X(BenchmarkAESCBCDecrypt1K)

TESTING_MAIN(TESTS)
