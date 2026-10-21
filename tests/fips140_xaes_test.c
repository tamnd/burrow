/* Derived from Go's src/crypto/internal/fips140test/xaes_test.go.
 * Go source: go1.27.1.
 *
 * Not in Go: Go calls the module's aes.New and gcm.New. The AES block that
 * keys the CounterKDF is the module's own here too, and the GCM is the public
 * cipher_new_gcm over aes_new_cipher, which run the same code.
 *
 * Copyright 2024 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/crypto/aes.h"
#include "burrow/crypto/cipher.h"
#include "burrow/encoding/hex.h"
#include "burrow/mem/heap.h"

#include "../src/crypto/aes_internal.h"
#include "../src/crypto/drbg_internal.h"
#include "../src/crypto/sha3_internal.h"

#include <stdint.h>
#include <string.h>

static Slice xaes_gcm(Alloc *a, Slice key, Slice nonce, CipherAEAD *g) {
    AesBlock c;
    burrow__aes_block_init(&c, (const Byte *)key.p, key.len);
    CounterKdf kdf;
    burrow__counter_kdf_init(&kdf, &c);
    Byte k[32];
    burrow__counter_kdf_derive_key(&kdf, k, 0x58, (const Byte *)nonce.p);
    CipherBlock b = aes_new_cipher(a, slice_from(k, 32, 32, TYPE_BYTE), NULL);
    *g = cipher_new_gcm(a, b, NULL);
    return slice_sub(nonce, 12, nonce.len);
}

static Slice xaes_seal(Alloc *a, Slice dst, Slice key, Slice nonce, Slice plaintext,
                       Slice additional_data) {
    CipherAEAD g;
    Slice n = xaes_gcm(a, key, nonce, &g);
    return cipher_aead_seal(g, a, dst, n, plaintext, additional_data);
}

static Slice xaes_open(Alloc *a, Slice dst, Slice key, Slice nonce, Slice ciphertext,
                       Slice additional_data, Error *err) {
    CipherAEAD g;
    Slice n = xaes_gcm(a, key, nonce, &g);
    return cipher_aead_open(g, a, dst, n, ciphertext, additional_data, err);
}

static uint64_t heap_allocs(void) {
    uint64_t n = 0, bytes = 0;
    burrow__heap_counts(&n, &bytes);
    return n;
}

static void xaes_allocations_run(TestingT *t, Alloc *a) {
    Byte key[32] = {0}, nonce[24] = {0}, plaintext[16] = {0}, aad[16] = {0};
    Byte ciphertext_data[16 + 16];
    Slice ciphertext = slice_from(ciphertext_data, 0, 16 + 16, TYPE_BYTE);
    ciphertext = xaes_seal(a, ciphertext, slice_from(key, 32, 32, TYPE_BYTE),
                           slice_from(nonce, 24, 24, TYPE_BYTE),
                           slice_from(plaintext, 16, 16, TYPE_BYTE),
                           slice_from(aad, 16, 16, TYPE_BYTE));
    Error err = BURROW_NO_ERROR;
    (void)xaes_open(a, slice_from(plaintext, 0, 16, TYPE_BYTE),
                    slice_from(key, 32, 32, TYPE_BYTE),
                    slice_from(nonce, 24, 24, TYPE_BYTE), ciphertext,
                    slice_from(aad, 16, 16, TYPE_BYTE), &err);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);
}

/* Not in Go: the AEAD and the blocks come from the allocator passed in, so
 * the count is of what comes from the heap besides, with an arena that is
 * reset after each run and so takes no more chunks once warmed up. Go's
 * AllocsPerRun does one run first and then divides by 100, and so does this.
 * The ppc64 skip and cryptotest.SkipTestAllocations are left out: they are
 * about allocations Go's compiler and sanitizers make, and burrow counts its
 * own. */
static void TestXAESAllocations(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    xaes_allocations_run(t, a);
    arena_reset(&ar);
    burrow__heap_count(true);
    uint64_t before = heap_allocs();
    for (int i = 0; i < 100; i++) {
        xaes_allocations_run(t, a);
        arena_reset(&ar);
    }
    uint64_t allocs = (heap_allocs() - before) / 100;
    burrow__heap_count(false);
    arena_free(&ar);
    if (allocs > 0)
        testing_t_errorf_v(t, "expected zero allocations, got %0.1f", (double)allocs);
}

static void TestXAES(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Byte key_data[32];
    memset(key_data, 0x01, sizeof key_data);
    Slice key = slice_from(key_data, 32, 32, TYPE_BYTE);
    Slice plaintext = slice_from_str(a, BURROW_S("XAES-256-GCM"));
    Slice additionalData = slice_from_str(a, BURROW_S("c2sp.org/XAES-256-GCM"));

    Byte nonce_data[24] = {0};
    Slice nonce = slice_from(nonce_data, 24, 24, TYPE_BYTE);
    Slice ciphertext = slice_make(a, TYPE_BYTE, plaintext.len + 16, plaintext.len + 16);

    burrow__drbg_read(slice_sub(nonce, 0, 12));
    AesBlock c;
    burrow__aes_block_init(&c, key_data, 32);
    CounterKdf kdf;
    burrow__counter_kdf_init(&kdf, &c);
    Byte k[32];
    burrow__counter_kdf_derive_key(&kdf, k, 0x58, nonce_data);
    CipherBlock b = aes_new_cipher(a, slice_from(k, 32, 32, TYPE_BYTE), NULL);
    /* Not in Go: gcm.SealWithRandomNonce(g, nonce[12:], ciphertext, plaintext,
     * additionalData) is the seal of the GCM with random nonces, whose output
     * is the nonce and then the ciphertext. */
    CipherAEAD g = cipher_new_gcm_with_random_nonce(a, b, NULL);
    Slice out = cipher_aead_seal(g, a, slice_nil(TYPE_BYTE), slice_nil(TYPE_BYTE),
                                 plaintext, additionalData);
    memcpy(nonce_data + 12, out.p, 12);
    memcpy(ciphertext.p, (Byte *)out.p + 12, (size_t)ciphertext.len);

    Error err = BURROW_NO_ERROR;
    Slice got = xaes_open(a, slice_nil(TYPE_BYTE), key, nonce, ciphertext,
                          additionalData, &err);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);
    if (!bytes_equal(plaintext, got))
        testing_t_errorf_v(t, "plaintext and got are not equal");
    arena_free(&ar);
}

static void TestXAESVectors(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Byte key_data[32];
    memset(key_data, 0x01, sizeof key_data);
    Slice key = slice_from(key_data, 32, 32, TYPE_BYTE);
    Slice nonce = slice_from_str(a, BURROW_S("ABCDEFGHIJKLMNOPQRSTUVWX"));
    Slice plaintext = slice_from_str(a, BURROW_S("XAES-256-GCM"));
    Slice ciphertext =
        xaes_seal(a, slice_nil(TYPE_BYTE), key, nonce, plaintext, slice_nil(TYPE_BYTE));
    Str expected = BURROW_S("ce546ef63c9cc60765923609b33a9a1974e96e52daf2fcf7075e2271");
    Str got = hex_encode_to_string(a, ciphertext);
    if (!str_eq(got, expected))
        testing_t_errorf_v(t, "got: %s", got);
    Error err = BURROW_NO_ERROR;
    Slice decrypted = xaes_open(a, slice_nil(TYPE_BYTE), key, nonce, ciphertext,
                                slice_nil(TYPE_BYTE), &err);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);
    else if (!bytes_equal(plaintext, decrypted))
        testing_t_errorf_v(t, "plaintext and decrypted are not equal");

    memset(key_data, 0x03, sizeof key_data);
    Slice aad = slice_from_str(a, BURROW_S("c2sp.org/XAES-256-GCM"));
    ciphertext = xaes_seal(a, slice_nil(TYPE_BYTE), key, nonce, plaintext, aad);
    expected = BURROW_S("986ec1832593df5443a179437fd083bf3fdb41abd740a21f71eb769d");
    got = hex_encode_to_string(a, ciphertext);
    if (!str_eq(got, expected))
        testing_t_errorf_v(t, "got: %s", got);
    decrypted = xaes_open(a, slice_nil(TYPE_BYTE), key, nonce, ciphertext, aad, &err);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);
    else if (!bytes_equal(plaintext, decrypted))
        testing_t_errorf_v(t, "plaintext and decrypted are not equal");
    arena_free(&ar);
}

static void TestXAESAccumulated(TestingT *t) {
    int iterations = 10000;
    Str expected =
        BURROW_S("e6b9edf2df6cec60c8cbd864e2211b597fb69a529160cd040d56c0c210081939");

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Sha3 s, d;
    burrow__shake128_init(&s);
    burrow__shake128_init(&d);
    for (int i = 0; i < iterations; i++) {
        Byte key[32];
        burrow__sha3_read(&s, key, 32);
        Byte nonce[24];
        burrow__sha3_read(&s, nonce, 24);
        Byte lenByte[1];
        burrow__sha3_read(&s, lenByte, 1);
        Byte plaintext[255];
        burrow__sha3_read(&s, plaintext, lenByte[0]);
        Int plaintext_len = lenByte[0];
        burrow__sha3_read(&s, lenByte, 1);
        Byte aad[255];
        burrow__sha3_read(&s, aad, lenByte[0]);
        Int aad_len = lenByte[0];

        Slice ciphertext =
            xaes_seal(a, slice_nil(TYPE_BYTE), slice_from(key, 32, 32, TYPE_BYTE),
                      slice_from(nonce, 24, 24, TYPE_BYTE),
                      slice_from(plaintext, plaintext_len, plaintext_len, TYPE_BYTE),
                      slice_from(aad, aad_len, aad_len, TYPE_BYTE));
        Error err = BURROW_NO_ERROR;
        Slice decrypted =
            xaes_open(a, slice_nil(TYPE_BYTE), slice_from(key, 32, 32, TYPE_BYTE),
                      slice_from(nonce, 24, 24, TYPE_BYTE), ciphertext,
                      slice_from(aad, aad_len, aad_len, TYPE_BYTE), &err);
        if (BURROW_FAILED(err))
            testing_t_fatal_v(t, err);
        if (!bytes_equal(slice_from(plaintext, plaintext_len, plaintext_len, TYPE_BYTE),
                         decrypted))
            testing_t_errorf_v(t, "plaintext and decrypted are not equal");

        burrow__sha3_write(&d, ciphertext.p, ciphertext.len);
        /* Not in Go: the memory of each round goes back here. */
        arena_reset(&ar);
    }
    /* Not in Go: d.Sum(nil) is the first 32 bytes read from d, which is what
     * Sum gives for SHAKE128 when nothing has been read. */
    Byte sum[32];
    burrow__sha3_read(&d, sum, 32);
    Str got = hex_encode_to_string(a, slice_from(sum, 32, 32, TYPE_BYTE));
    if (!str_eq(got, expected))
        testing_t_errorf_v(t, "got: %s", got);
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestXAESAllocations)                                                             \
    X(TestXAES)                                                                        \
    X(TestXAESVectors)                                                                 \
    X(TestXAESAccumulated)

TESTING_MAIN(TESTS)
