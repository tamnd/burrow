/* Derived from Go's src/crypto/aes/aes_test.go.
 * Go source: go1.27.1.
 *
 * TestExtraMethods is left out. It asks reflection whether the block has any
 * exported methods beyond cipher.Block's, and a CipherBlock is a table with
 * exactly those three in it.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"
#include "testcipher.h"

#include "burrow/burrow.h"
#include "burrow/crypto/aes.h"

#include <string.h>

/* Test vectors are from FIPS 197, appendices B and C. */
typedef struct CryptTest {
    Int key_len;
    Byte key[32];
    Byte in[16];
    Byte out[16];
} CryptTest;

static const CryptTest encrypt_tests[] = {
    {
        /* Appendix B. */
        16,
        {0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6, 0xab, 0xf7, 0x15, 0x88, 0x09,
         0xcf, 0x4f, 0x3c},
        {0x32, 0x43, 0xf6, 0xa8, 0x88, 0x5a, 0x30, 0x8d, 0x31, 0x31, 0x98, 0xa2, 0xe0,
         0x37, 0x07, 0x34},
        {0x39, 0x25, 0x84, 0x1d, 0x02, 0xdc, 0x09, 0xfb, 0xdc, 0x11, 0x85, 0x97, 0x19,
         0x6a, 0x0b, 0x32},
    },
    {
        /* Appendix C.1. AES-128 */
        16,
        {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c,
         0x0d, 0x0e, 0x0f},
        {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc,
         0xdd, 0xee, 0xff},
        {0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b, 0x04, 0x30, 0xd8, 0xcd, 0xb7, 0x80, 0x70,
         0xb4, 0xc5, 0x5a},
    },
    {
        /* Appendix C.2. AES-192 */
        24,
        {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b,
         0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17},
        {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc,
         0xdd, 0xee, 0xff},
        {0xdd, 0xa9, 0x7c, 0xa4, 0x86, 0x4c, 0xdf, 0xe0, 0x6e, 0xaf, 0x70, 0xa0, 0xec,
         0x0d, 0x71, 0x91},
    },
    {
        /* Appendix C.3. AES-256 */
        32,
        {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a,
         0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15,
         0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f},
        {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc,
         0xdd, 0xee, 0xff},
        {0x8e, 0xa2, 0xb7, 0xca, 0x51, 0x67, 0x45, 0xbf, 0xea, 0xfc, 0x49, 0x90, 0x4b,
         0x49, 0x60, 0x89},
    },
};

#define NTESTS ((Int)(sizeof encrypt_tests / sizeof encrypt_tests[0]))

static Slice bs(const Byte *p, Int n) {
    return slice_from((void *)(uintptr_t)p, n, n, TYPE_BYTE);
}

/* Test Cipher Encrypt method against FIPS 197 examples. */
static void test_cipher_encrypt(void *env, TestingT *t) {
    (void)env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < NTESTS; i++) {
        const CryptTest *tt = &encrypt_tests[i];
        Error err = BURROW_NO_ERROR;
        CipherBlock c = aes_new_cipher(a, bs(tt->key, tt->key_len), &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "NewCipher(%d bytes) = %v", tt->key_len, err);
            continue;
        }
        Byte out[16];
        cipher_block_encrypt(c, bs(out, 16), bs(tt->in, 16));
        for (Int j = 0; j < 16; j++) {
            if (out[j] != tt->out[j]) {
                testing_t_errorf_v(t, "Cipher.Encrypt %d: out[%d] = %#x, want %#x", i,
                                   j, out[j], tt->out[j]);
                break;
            }
        }
    }
    arena_free(&ar);
}

static void TestCipherEncrypt(TestingT *t) {
    testcipher_all_implementations(t, test_cipher_encrypt, NULL);
}

/* Test Cipher Decrypt against FIPS 197 examples. */
static void test_cipher_decrypt(void *env, TestingT *t) {
    (void)env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < NTESTS; i++) {
        const CryptTest *tt = &encrypt_tests[i];
        Error err = BURROW_NO_ERROR;
        CipherBlock c = aes_new_cipher(a, bs(tt->key, tt->key_len), &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "NewCipher(%d bytes) = %v", tt->key_len, err);
            continue;
        }
        Byte plain[16];
        cipher_block_decrypt(c, bs(plain, 16), bs(tt->out, 16));
        for (Int j = 0; j < 16; j++) {
            if (plain[j] != tt->in[j]) {
                testing_t_errorf_v(t, "decryptBlock %d: plain[%d] = %#x, want %#x", i,
                                   j, plain[j], tt->in[j]);
                break;
            }
        }
    }
    arena_free(&ar);
}

static void TestCipherDecrypt(TestingT *t) {
    testcipher_all_implementations(t, test_cipher_decrypt, NULL);
}

static void aes_block_keylen(void *env, TestingT *t) {
    testcipher_block(t, *(const Int *)env / 8, aes_new_cipher);
}

/* Test AES against the general cipher.Block interface tester. */
static void test_aes_block(void *env, TestingT *t) {
    (void)env;
    static const Int keylens[] = {128, 192, 256};
    for (size_t i = 0; i < sizeof keylens / sizeof keylens[0]; i++) {
        Str name = keylens[i] == 128   ? BURROW_S("AES-128")
                   : keylens[i] == 192 ? BURROW_S("AES-192")
                                       : BURROW_S("AES-256");
        testing_t_run(
            t, name,
            BURROW_FN(TestingTFunc, aes_block_keylen, (void *)(uintptr_t)&keylens[i]));
    }
}

static void TestAESBlock(TestingT *t) {
    testcipher_all_implementations(t, test_aes_block, NULL);
}

/* Not Go's: the key sizes NewCipher refuses, and its error for them. */
static void TestKeySizeError(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    static const Byte key[33] = {0};
    static const Int bad[] = {0, 1, 15, 17, 23, 25, 31, 33};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        Error err = BURROW_NO_ERROR;
        (void)aes_new_cipher(a, bs(key, bad[i]), &err);
        if (!BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "NewCipher(%d bytes) succeeded", bad[i]);
            continue;
        }
        Str want = fmt_sprintf_v(a, "crypto/aes: invalid key size %d", bad[i]);
        Str got = error_text(err);
        if (!str_eq(got, want))
            testing_t_errorf_v(t, "NewCipher(%d bytes) = %q, want %q", bad[i], got,
                               want);
    }
    arena_free(&ar);
}

/* --------------------------------------------------------------- benchmarks */

static void benchmark_encrypt(void *env, TestingB *b) {
    const CryptTest *tt = env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Error err = BURROW_NO_ERROR;
    CipherBlock c =
        aes_new_cipher(arena_allocator(&ar), bs(tt->key, tt->key_len), &err);
    if (BURROW_FAILED(err))
        testing_b_fatal_v(b, "NewCipher:", err);
    Byte out[16];
    testing_b_set_bytes(b, 16);
    testing_b_reset_timer(b);
    for (Int i = 0; i < testing_b_n(b); i++)
        cipher_block_encrypt(c, bs(out, 16), bs(tt->in, 16));
    arena_free(&ar);
}

static void benchmark_decrypt(void *env, TestingB *b) {
    const CryptTest *tt = env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Error err = BURROW_NO_ERROR;
    CipherBlock c =
        aes_new_cipher(arena_allocator(&ar), bs(tt->key, tt->key_len), &err);
    if (BURROW_FAILED(err))
        testing_b_fatal_v(b, "NewCipher:", err);
    Byte out[16];
    testing_b_set_bytes(b, 16);
    testing_b_reset_timer(b);
    for (Int i = 0; i < testing_b_n(b); i++)
        cipher_block_decrypt(c, bs(out, 16), bs(tt->out, 16));
    arena_free(&ar);
}

static void benchmark_create_cipher(void *env, TestingB *b) {
    const CryptTest *tt = env;
    testing_b_report_allocs(b);
    Arena ar;
    arena_init(&ar, NULL, 0);
    for (Int i = 0; i < testing_b_n(b); i++) {
        Error err = BURROW_NO_ERROR;
        (void)aes_new_cipher(arena_allocator(&ar), bs(tt->key, tt->key_len), &err);
        if (BURROW_FAILED(err))
            testing_b_fatal_v(b, err);
        arena_reset(&ar);
    }
    arena_free(&ar);
}

static void bench_sizes(TestingB *b, void (*fn)(void *, TestingB *)) {
    testing_b_run(b, BURROW_S("AES-128"),
                  BURROW_FN(TestingBFunc, fn, (void *)(uintptr_t)&encrypt_tests[1]));
    testing_b_run(b, BURROW_S("AES-192"),
                  BURROW_FN(TestingBFunc, fn, (void *)(uintptr_t)&encrypt_tests[2]));
    testing_b_run(b, BURROW_S("AES-256"),
                  BURROW_FN(TestingBFunc, fn, (void *)(uintptr_t)&encrypt_tests[3]));
}

static void BenchmarkEncrypt(TestingB *b) {
    bench_sizes(b, benchmark_encrypt);
}

static void BenchmarkDecrypt(TestingB *b) {
    bench_sizes(b, benchmark_decrypt);
}

static void BenchmarkCreateCipher(TestingB *b) {
    bench_sizes(b, benchmark_create_cipher);
}

#define TESTS(X)                                                                       \
    X(TestCipherEncrypt)                                                               \
    X(TestCipherDecrypt)                                                               \
    X(TestAESBlock)                                                                    \
    X(TestKeySizeError)                                                                \
    X(BenchmarkEncrypt)                                                                \
    X(BenchmarkDecrypt)                                                                \
    X(BenchmarkCreateCipher)

TESTING_MAIN(TESTS)
