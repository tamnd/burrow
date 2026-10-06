/* Derived from Go's src/crypto/hmac/hmac_test.go.
 * Go source: go1.27.1.
 *
 * The table is Go's, keys written as hex. Go runs the table a second time
 * through a wrapper that hides the hashes' MarshalBinary, which changes the
 * path Reset takes there; nothing marshals yet here (#185), so that round
 * takes the same path as the first and is kept to match Go's loop. TestNoClone
 * and TestExtraMethods are about hash.Cloner and Go's method sets, which a
 * Hash here does not have.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"
#include "testhash.h"

#include "burrow/burrow.h"
#include "burrow/crypto/hmac.h"
#include "burrow/crypto/md5.h"
#include "burrow/crypto/sha1.h"
#include "burrow/crypto/sha256.h"
#include "burrow/crypto/sha3.h"
#include "burrow/crypto/sha512.h"
#include "burrow/encoding/hex.h"

#include <stdint.h>
#include <string.h>

typedef struct HmacTest {
    HashNewFunc hash;
    const char *key;
    const char *in;
    Int in_len;
    const char *out;
    Int size;
    Int blocksize;
} HmacTest;

#define NELEM(x) (sizeof(x) / sizeof((x)[0]))

static const HmacTest hmac_tests[] = {
    {sha1_new,
     "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
     "202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f",
     "Sample #1", 9, "4f4ca3d5d68ba7cc0a1208c9c61e9c5da0403c0a", SHA1_SIZE,
     SHA1_BLOCK_SIZE},
    {sha1_new, "303132333435363738393a3b3c3d3e3f40414243", "Sample #2", 9,
     "0922d3405faa3d194f82a45830737d5cc6c75d24", SHA1_SIZE, SHA1_BLOCK_SIZE},
    {sha1_new,
     "505152535455565758595a5b5c5d5e5f606162636465666768696a6b6c6d6e6f"
     "707172737475767778797a7b7c7d7e7f808182838485868788898a8b8c8d8e8f"
     "909192939495969798999a9b9c9d9e9fa0a1a2a3a4a5a6a7a8a9aaabacadaeaf"
     "b0b1b2b3",
     "Sample #3", 9, "bcf41eab8bb2d802f3d05caf7cb092ecf8d1a3aa", SHA1_SIZE,
     SHA1_BLOCK_SIZE},
    {md5_new, "4a656665", "what do ya want for nothing?", 28,
     "750c783e6ab0b503eaa86e310a5db738", MD5_SIZE, MD5_BLOCK_SIZE},
    {sha256_new, "0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b", "Hi There", 8,
     "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7", SHA256_SIZE,
     SHA256_BLOCK_SIZE},
    {sha256_new, "4a656665", "what do ya want for nothing?", 28,
     "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843", SHA256_SIZE,
     SHA256_BLOCK_SIZE},
    {sha256_new, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
     "\335\335\335\335\335\335\335\335\335\335\335\335\335\335\335\335"
     "\335\335\335\335\335\335\335\335\335\335\335\335\335\335\335\335"
     "\335\335\335\335\335\335\335\335\335\335\335\335\335\335\335\335"
     "\335\335",
     50, "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe",
     SHA256_SIZE, SHA256_BLOCK_SIZE},
    {sha256_new, "0102030405060708090a0b0c0d0e0f10111213141516171819",
     "\315\315\315\315\315\315\315\315\315\315\315\315\315\315\315\315"
     "\315\315\315\315\315\315\315\315\315\315\315\315\315\315\315\315"
     "\315\315\315\315\315\315\315\315\315\315\315\315\315\315\315\315"
     "\315\315",
     50, "82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b",
     SHA256_SIZE, SHA256_BLOCK_SIZE},
    {sha256_new,
     "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
     "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
     "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
     "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
     "aaaaaa",
     "Test Using Larger Than Block-Size Key - Hash Key First", 54,
     "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54", SHA256_SIZE,
     SHA256_BLOCK_SIZE},
    {sha256_new,
     "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
     "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
     "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
     "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
     "aaaaaa",
     "This is a test using a larger than block-size key and a larger t"
     "han block-size data. The key needs to be hashed before being use"
     "d by the HMAC algorithm.",
     152, "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2",
     SHA256_SIZE, SHA256_BLOCK_SIZE},
    {sha1_new,
     "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
     "202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f",
     "Sample message for keylen=blocklen", 34,
     "5fd596ee78d5553c8ff4e72d266dfd192366da29", SHA1_SIZE, SHA1_BLOCK_SIZE},
    {sha1_new, "000102030405060708090a0b0c0d0e0f10111213",
     "Sample message for keylen<blocklen", 34,
     "4c99ff0cb1b31bd33f8431dbaf4d17fcd356a807", SHA1_SIZE, SHA1_BLOCK_SIZE},
    {sha1_new,
     "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
     "202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f"
     "404142434445464748494a4b4c4d4e4f505152535455565758595a5b5c5d5e5f"
     "60616263",
     "Sample message for keylen=blocklen", 34,
     "2d51b2f7750e410584662e38f133435f4c4fd42a", SHA1_SIZE, SHA1_BLOCK_SIZE},
    {sha256_new224,
     "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
     "202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f",
     "Sample message for keylen=blocklen", 34,
     "c7405e3ae058e8cd30b08b4140248581ed174cb34e1224bcc1efc81b", SHA256_SIZE224,
     SHA256_BLOCK_SIZE},
    {sha256_new224, "000102030405060708090a0b0c0d0e0f101112131415161718191a1b",
     "Sample message for keylen<blocklen", 34,
     "e3d249a8cfb67ef8b7a169e9a0a599714a2cecba65999a51beb8fbbe", SHA256_SIZE224,
     SHA256_BLOCK_SIZE},
    {sha256_new224,
     "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
     "202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f"
     "404142434445464748494a4b4c4d4e4f505152535455565758595a5b5c5d5e5f"
     "60616263",
     "Sample message for keylen=blocklen", 34,
     "91c52509e5af8531601ae6230099d90bef88aaefb961f4080abc014d", SHA256_SIZE224,
     SHA256_BLOCK_SIZE},
    {sha256_new,
     "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
     "202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f",
     "Sample message for keylen=blocklen", 34,
     "8bb9a1db9806f20df7f77b82138c7914d174d59e13dc4d0169c9057b133e1d62", SHA256_SIZE,
     SHA256_BLOCK_SIZE},
    {sha256_new, "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f",
     "Sample message for keylen<blocklen", 34,
     "a28cf43130ee696a98f14a37678b56bcfcbdd9e5cf69717fecf5480f0ebdf790", SHA256_SIZE,
     SHA256_BLOCK_SIZE},
    {sha256_new,
     "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
     "202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f"
     "404142434445464748494a4b4c4d4e4f505152535455565758595a5b5c5d5e5f"
     "60616263",
     "Sample message for keylen=blocklen", 34,
     "bdccb6c72ddeadb500ae768386cb38cc41c63dbb0878ddb9c7a38a431b78378d", SHA256_SIZE,
     SHA256_BLOCK_SIZE},
    {sha512_new384,
     "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
     "202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f"
     "404142434445464748494a4b4c4d4e4f505152535455565758595a5b5c5d5e5f"
     "606162636465666768696a6b6c6d6e6f707172737475767778797a7b7c7d7e7f",
     "Sample message for keylen=blocklen", 34,
     "63c5daa5e651847ca897c95814ab830bededc7d25e83eef9195cd45857a37f448947858f5af50cc2b"
     "1b730ddf29671a9",
     SHA512_SIZE384, SHA512_BLOCK_SIZE},
    {sha512_new384,
     "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
     "202122232425262728292a2b2c2d2e2f",
     "Sample message for keylen<blocklen", 34,
     "6eb242bdbb582ca17bebfa481b1e23211464d2b7f8c20b9ff2201637b93646af5ae9ac316e98db45d"
     "9cae773675eeed0",
     SHA512_SIZE384, SHA512_BLOCK_SIZE},
    {sha512_new384,
     "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
     "202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f"
     "404142434445464748494a4b4c4d4e4f505152535455565758595a5b5c5d5e5f"
     "606162636465666768696a6b6c6d6e6f707172737475767778797a7b7c7d7e7f"
     "808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f"
     "a0a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3b4b5b6b7b8b9babbbcbdbebf"
     "c0c1c2c3c4c5c6c7",
     "Sample message for keylen=blocklen", 34,
     "5b664436df69b0ca22551231a3f0a3d5b4f97991713cfa84bff4d0792eff96c27dccbbb6f79b65d54"
     "8b40e8564cef594",
     SHA512_SIZE384, SHA512_BLOCK_SIZE},
    {sha512_new,
     "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
     "202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f"
     "404142434445464748494a4b4c4d4e4f505152535455565758595a5b5c5d5e5f"
     "606162636465666768696a6b6c6d6e6f707172737475767778797a7b7c7d7e7f",
     "Sample message for keylen=blocklen", 34,
     "fc25e240658ca785b7a811a8d3f7b4ca48cfa26a8a366bf2cd1f836b05fcb024bd36853081811d6ce"
     "a4216ebad79da1cfcb95ea4586b8a0ce356596a55fb1347",
     SHA512_SIZE, SHA512_BLOCK_SIZE},
    {sha512_new,
     "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
     "202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f",
     "Sample message for keylen<blocklen", 34,
     "fd44c18bda0bb0a6ce0e82b031bf2818f6539bd56ec00bdc10a8a2d730b3634de2545d639b0f2cf71"
     "0d0692c72a1896f1f211c2b922d1a96c392e07e7ea9fedc",
     SHA512_SIZE, SHA512_BLOCK_SIZE},
    {sha512_new,
     "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
     "202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f"
     "404142434445464748494a4b4c4d4e4f505152535455565758595a5b5c5d5e5f"
     "606162636465666768696a6b6c6d6e6f707172737475767778797a7b7c7d7e7f"
     "808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f"
     "a0a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3b4b5b6b7b8b9babbbcbdbebf"
     "c0c1c2c3c4c5c6c7",
     "Sample message for keylen=blocklen", 34,
     "d93ec8d2de1ad2a9957cb9b83f14e76ad6b5e0cce285079a127d3b14bccb7aa7286d4ac0d4ce64215"
     "f2bc9e6870b33d97438be4aaa20cda5c5a912b48b8e27f3",
     SHA512_SIZE, SHA512_BLOCK_SIZE},
    {sha1_new, "", "message", 7, "d5d1ed05121417247616cfc8378f360a39da7cfa", SHA1_SIZE,
     SHA1_BLOCK_SIZE},
    {sha256_new, "", "message", 7,
     "eb08c1f56d5ddee07f7bdf80468083da06b64cf4fac64fe3a90883df5feacae4", SHA256_SIZE,
     SHA256_BLOCK_SIZE},
    {sha512_new, "", "message", 7,
     "08fce52f6395d59c2a3fb8abb281d74ad6f112b9a9c787bcea290d94dadbc82b2ca3e5e12bf2277c7"
     "fedbb0154d5493e41bb7459f63c8e39554ea3651b812492",
     SHA512_SIZE, SHA512_BLOCK_SIZE},
};

static Slice unhex(Alloc *a, const char *s) {
    Slice b = hex_decode_string(a, str_from_cstr(s), NULL);
    if (b.elem == NULL)
        b = slice_nil(TYPE_BYTE);
    return b;
}

static Slice in_bytes(const HmacTest *tt) {
    return slice_from((void *)(uintptr_t)tt->in, tt->in_len, tt->in_len, TYPE_BYTE);
}

static void TestHMAC(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < NELEM(hmac_tests); i++) {
        const HmacTest *tt = &hmac_tests[i];
        Slice key = unhex(a, tt->key);
        Hash h = hmac_new(a, tt->hash, key);
        Int s = hash_size(h);
        if (s != tt->size)
            testing_t_errorf_v(t, "Size: got %d, want %d", s, tt->size);
        Int b = hash_block_size(h);
        if (b != tt->blocksize)
            testing_t_errorf_v(t, "BlockSize: got %d, want %d", b, tt->blocksize);
        for (int j = 0; j < 4; j++) {
            Error err;
            Int n = hash_write(h, in_bytes(tt), &err);
            if (n != tt->in_len || BURROW_FAILED(err)) {
                testing_t_errorf_v(t, "test %d.%d: Write(%d) = %d, %s", (Int)i, (Int)j,
                                   tt->in_len, n, error_text(err));
                continue;
            }

            /* Repeated Sums give the same value. */
            for (int k = 0; k < 2; k++) {
                Str sum = hex_encode_to_string(a, hash_sum(a, h, slice_nil(TYPE_BYTE)));
                if (!str_eq(sum, str_from_cstr(tt->out)))
                    testing_t_errorf_v(t, "test %d.%d.%d: have %s want %s\n", (Int)i,
                                       (Int)j, (Int)k, sum, str_from_cstr(tt->out));
            }

            /* Second time round: make sure reset works. */
            hash_reset(h);

            if (j == 1)
                h = hmac_new(a, tt->hash, key);
        }
    }
    arena_free(&ar);
}

static Hash new_sha3_256(Alloc *a) {
    Sha3 *d = sha3_new256(a);
    if (d == NULL)
        return (Hash){NULL, NULL};
    return sha3_as_hash(d);
}

/* Go builds an HMAC over a zero SHA3 value here, which once crashed it. The
 * nearest thing is the plain constructor, and a sum of the right length. */
static void TestSHA3Hash(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Hash h = hmac_new(a, new_sha3_256,
                      slice_from((void *)(uintptr_t)"key", 3, 3, TYPE_BYTE));
    CHECK(h.vt != NULL);
    CHECK_INT_EQ(hash_sum(a, h, slice_nil(TYPE_BYTE)).len, 32);
    CHECK_INT_EQ(hash_block_size(h), 136);
    arena_free(&ar);
}

static Hash the_one_sha;

static Hash same_sha(Alloc *a) {
    (void)a;
    return the_one_sha;
}

static Str panic_message(Alloc *a, HashNewFunc f) {
    static char msg[128];
    volatile Int n = -1;
    BURROW_TRY {
        hmac_new(a, f, slice_from((void *)(uintptr_t)"bytes", 5, 5, TYPE_BYTE));
    }
    BURROW_CATCH(r) {
        Str s = panic_text(r);
        n = s.len < (Int)sizeof msg ? s.len : (Int)sizeof msg;
        memcpy(msg, s.p, (size_t)n);
    }
    BURROW_TRY_END;
    if (n < 0)
        return BURROW_S("no panic");
    return str_from_bytes((const Byte *)msg, n);
}

static void TestNonUniqueHash(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    the_one_sha = sha256_new(a);
    Str got = panic_message(a, same_sha);
    if (!str_eq(got, BURROW_S("crypto/hmac: hash generation function does not produce "
                              "unique values")))
        testing_t_errorf_v(t,
                           "expected panic when calling New with a non-unique hash "
                           "generation function, got %q",
                           got);
    arena_free(&ar);
}

static Slice sb(const char *s) {
    Int n = (Int)strlen(s);
    return slice_from((void *)(uintptr_t)s, n, n, TYPE_BYTE);
}

static void TestEqual(TestingT *t) {
    Slice a = sb("test");
    Slice b = sb("test1");
    Slice c = sb("test2");

    if (!hmac_equal(b, b))
        testing_t_errorf_v(t, "Equal failed with equal arguments");
    if (hmac_equal(a, b))
        testing_t_errorf_v(t, "Equal accepted a prefix of the second argument");
    if (hmac_equal(b, a))
        testing_t_errorf_v(t, "Equal accepted a prefix of the first argument");
    if (hmac_equal(b, c))
        testing_t_errorf_v(t, "Equal accepted unequal slices");
}

/* testhash takes a function of the allocator alone, so the test case it is
 * working on rides in here. The subtests run one at a time. */
static const HmacTest *current;

static Hash new_current(Alloc *a) {
    return hmac_new(a, current->hash, unhex(a, current->key));
}

static void run_testhash(void *env, TestingT *t) {
    current = env;
    testhash_without_clone(t, new_current);
}

static void TestHMACHash(TestingT *t) {
    char name[32];
    for (size_t i = 0; i < NELEM(hmac_tests); i++) {
        snprintf(name, sizeof name, "test-%d", (int)i);
        testing_t_run(
            t, str_from_cstr(name),
            BURROW_FN(TestingTFunc, run_testhash, (void *)(uintptr_t)&hmac_tests[i]));
    }
}

static void bench_sha256(TestingB *b, Int size) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice key = slice_make(a, TYPE_BYTE, 32, 32);
    Slice buf = slice_make(a, TYPE_BYTE, size, size);
    Slice out = slice_make(a, TYPE_BYTE, 0, 32);
    Hash h = hmac_new(a, sha256_new, key);
    testing_b_set_bytes(b, size);
    for (Int i = 0; i < testing_b_n(b); i++) {
        hash_write(h, buf, NULL);
        hash_reset(h);
        Slice mac = hash_sum(a, h, slice_sub(out, 0, 0));
        ((Byte *)buf.p)[0] = ((Byte *)mac.p)[0];
    }
    arena_free(&ar);
}

static void BenchmarkHMACSHA256_1K(TestingB *b) {
    bench_sha256(b, 1024);
}

static void BenchmarkHMACSHA256_32(TestingB *b) {
    bench_sha256(b, 32);
}

static void BenchmarkNewWriteSum(TestingB *b) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice key = slice_make(a, TYPE_BYTE, 32, 32);
    Slice buf = slice_make(a, TYPE_BYTE, 1, 1);
    testing_b_set_bytes(b, 1);
    for (Int i = 0; i < testing_b_n(b); i++) {
        /* An arena only grows, so start it over now and then. */
        if (i % 4096 == 0) {
            arena_reset(&ar);
            key = slice_make(a, TYPE_BYTE, 32, 32);
            buf = slice_make(a, TYPE_BYTE, 1, 1);
        }
        Hash h = hmac_new(a, sha256_new, key);
        hash_write(h, buf, NULL);
        Slice mac = hash_sum(a, h, slice_nil(TYPE_BYTE));
        ((Byte *)buf.p)[0] = ((Byte *)mac.p)[0];
    }
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestHMAC)                                                                        \
    X(TestSHA3Hash)                                                                    \
    X(TestNonUniqueHash)                                                               \
    X(TestEqual)                                                                       \
    X(TestHMACHash)                                                                    \
    X(BenchmarkHMACSHA256_1K)                                                          \
    X(BenchmarkHMACSHA256_32)                                                          \
    X(BenchmarkNewWriteSum)

TESTING_MAIN(TESTS)
