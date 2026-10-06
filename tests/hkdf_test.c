/* Derived from Go's src/crypto/hkdf/hkdf_test.go.
 * Go source: go1.27.1.
 *
 * The table is Go's, with the bytes written as hex and a nil salt or info as
 * NULL. TestFIPSServiceIndicator is about Go's FIPS module, which is not here.
 * TestHKDFWycheproof waits for the Wycheproof vectors, which are fetched in CI
 * rather than kept in the tree.
 *
 * Copyright 2014 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/crypto/hkdf.h"
#include "burrow/crypto/md5.h"
#include "burrow/crypto/sha1.h"
#include "burrow/crypto/sha256.h"
#include "burrow/crypto/sha512.h"
#include "burrow/encoding/hex.h"

#include <stdint.h>
#include <string.h>

typedef struct HkdfTest {
    HashNewFunc hash;
    const char *master;
    const char *salt;
    const char *prk;
    const char *info;
    const char *out;
} HkdfTest;

#define NELEM(x) (sizeof(x) / sizeof((x)[0]))

static const HkdfTest hkdf_tests[] = {
    {sha256_new, "0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b",
     "000102030405060708090a0b0c",
     "077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5",
     "f0f1f2f3f4f5f6f7f8f9",
     "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf"
     "34007208d5b887185865"},
    {sha256_new,
     "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
     "202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f"
     "404142434445464748494a4b4c4d4e4f",
     "606162636465666768696a6b6c6d6e6f707172737475767778797a7b7c7d7e7f"
     "808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f"
     "a0a1a2a3a4a5a6a7a8a9aaabacadaeaf",
     "06a6b88c5853361a06104c9ceb35b45cef760014904671014a193f40c15fc244",
     "b0b1b2b3b4b5b6b7b8b9babbbcbdbebfc0c1c2c3c4c5c6c7c8c9cacbcccdcecf"
     "d0d1d2d3d4d5d6d7d8d9dadbdcdddedfe0e1e2e3e4e5e6e7e8e9eaebecedeeef"
     "f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff",
     "b11e398dc80327a1c8e7f78c596a49344f012eda2d4efad8a050cc4c19afa97c"
     "59045a99cac7827271cb41c65e590e09da3275600c2f09b8367793a9aca3db71"
     "cc30c58179ec3e87c14c01d5c1f3434f1d87"},
    {sha256_new, "0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b", "",
     "19ef24a32c717b167f33a91d6f648bdf96596776afdb6377ac434c1c293ccb04", "",
     "8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d"
     "9d201395faa4b61a96c8"},
    {sha256_new, "0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b", NULL,
     "19ef24a32c717b167f33a91d6f648bdf96596776afdb6377ac434c1c293ccb04", NULL,
     "8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d"
     "9d201395faa4b61a96c8"},
    {sha1_new, "0b0b0b0b0b0b0b0b0b0b0b", "000102030405060708090a0b0c",
     "9b6c18c432a7bf8f0e71c8eb88f4b30baa2ba243", "f0f1f2f3f4f5f6f7f8f9",
     "085a01ea1b10f36933068b56efa5ad81a4f14b822f5b091568a9cdd4f155fda2"
     "c22e422478d305f3f896"},
    {sha1_new,
     "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
     "202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f"
     "404142434445464748494a4b4c4d4e4f",
     "606162636465666768696a6b6c6d6e6f707172737475767778797a7b7c7d7e7f"
     "808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f"
     "a0a1a2a3a4a5a6a7a8a9aaabacadaeaf",
     "8adae09a2a307059478d309b26c4115a224cfaf6",
     "b0b1b2b3b4b5b6b7b8b9babbbcbdbebfc0c1c2c3c4c5c6c7c8c9cacbcccdcecf"
     "d0d1d2d3d4d5d6d7d8d9dadbdcdddedfe0e1e2e3e4e5e6e7e8e9eaebecedeeef"
     "f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff",
     "0bd770a74d1160f7c9f12cd5912a06ebff6adcae899d92191fe4305673ba2ffe"
     "8fa3f1a4e5ad79f3f334b3b202b2173c486ea37ce3d397ed034c7f9dfeb15c5e"
     "927336d0441f4c4300e2cff0d0900b52d3b4"},
    {sha1_new, "0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b", "",
     "da8c8a73c7fa77288ec6f5e7c297786aa0d32d01", "",
     "0ac1af7002b3d761d1e55298da9d0506b9ae52057220a306e07b6b87e8df21d0"
     "ea00033de03984d34918"},
    {sha1_new, "0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c", NULL,
     "2adccada18779e7c2077ad2eb19d3f3e731385dd", NULL,
     "2c91117204d745f3500d636a62f64f0ab3bae548aa53d423b0d1f27ebba6f5e5"
     "673a081d70cce7acfc48"},
};

/* NULL is Go's nil, which is the nil slice here. */
static Slice unhex(Alloc *a, const char *s) {
    if (s == NULL)
        return slice_nil(TYPE_BYTE);
    return hex_decode_string(a, str_from_cstr(s), NULL);
}

static Str as_str(Slice b) {
    return str_from_bytes((const Byte *)b.p, b.len);
}

static bool same(Slice a, Slice b) {
    return a.len == b.len && (a.len == 0 || memcmp(a.p, b.p, (size_t)a.len) == 0);
}

static void TestHKDF(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < NELEM(hkdf_tests); i++) {
        const HkdfTest *tt = &hkdf_tests[i];
        Slice master = unhex(a, tt->master);
        Slice salt = unhex(a, tt->salt);
        Slice want_prk = unhex(a, tt->prk);
        Str info = as_str(unhex(a, tt->info));
        Slice want = unhex(a, tt->out);
        Error err;

        Slice prk = hkdf_extract(a, tt->hash, master, salt, &err);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "test %d: PRK extraction failed: %s", (Int)i,
                               error_text(err));
        if (!same(prk, want_prk))
            testing_t_errorf_v(t, "test %d: incorrect PRK: have %x, need %x.", (Int)i,
                               as_str(prk), as_str(want_prk));

        Slice key = hkdf_key(a, tt->hash, master, salt, info, want.len, &err);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "test %d: key derivation failed: %s", (Int)i,
                               error_text(err));
        if (!same(key, want))
            testing_t_errorf_v(t, "test %d: incorrect output: have %x, need %x.",
                               (Int)i, as_str(key), as_str(want));

        Slice expanded = hkdf_expand(a, tt->hash, prk, info, want.len, &err);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "test %d: key expansion failed: %s", (Int)i,
                               error_text(err));
        if (!same(expanded, want))
            testing_t_errorf_v(
                t, "test %d: incorrect output from Expand: have %x, need %x.", (Int)i,
                as_str(expanded), as_str(want));
    }
    arena_free(&ar);
}

static void want_error(TestingT *t, Error err, const char *want) {
    if (!str_eq(error_text(err), str_from_cstr(want)))
        testing_t_errorf_v(t, "error %q, want %q", error_text(err),
                           str_from_cstr(want));
}

static void TestHKDFLimit(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    static Byte master_bytes[] = {0x00, 0x01, 0x02, 0x03};
    Slice master = slice_from(master_bytes, 4, 4, TYPE_BYTE);
    Str info = BURROW_S("");
    Int limit = SHA1_SIZE * 255;
    Error err;

    /* The most output there is can be had. */
    Slice out = hkdf_key(a, sha1_new, master, slice_nil(TYPE_BYTE), info, limit, &err);
    if (BURROW_FAILED(err) || out.len != limit)
        testing_t_errorf_v(t, "key derivation failed: %s", error_text(err));

    /* One more byte is an error. */
    hkdf_key(a, sha1_new, master, slice_nil(TYPE_BYTE), info, limit + 1, &err);
    if (BURROW_OK(err))
        testing_t_errorf_v(t, "expected key derivation to fail, but it succeeded");
    else
        want_error(t, err, "hkdf: requested key length too large");
    hkdf_expand(a, sha1_new, master, info, limit + 1, &err);
    if (BURROW_OK(err))
        testing_t_errorf_v(t, "expected key expansion to fail, but it succeeded");
    arena_free(&ar);
}

static void benchmark_hkdf(TestingB *b, HashNewFunc hasher, Int block) {
    static Byte master_bytes[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    static Byte salt_bytes[] = {0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17};
    static const Byte info_bytes[] = {0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27};
    Slice master = slice_from(master_bytes, 8, 8, TYPE_BYTE);
    Slice salt = slice_from(salt_bytes, 8, 8, TYPE_BYTE);
    Str info = str_from_bytes(info_bytes, 8);
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    testing_b_set_bytes(b, block);
    for (Int i = 0; i < testing_b_n(b); i++) {
        /* An arena only grows, so start it over now and then. */
        if (i % 1024 == 0)
            arena_reset(&ar);
        Error err;
        hkdf_key(a, hasher, master, salt, info, block, &err);
        if (BURROW_FAILED(err))
            testing_b_errorf_v(b, "failed to derive key: %s", error_text(err));
    }
    arena_free(&ar);
}

static void Benchmark16ByteMD5Single(TestingB *b) {
    benchmark_hkdf(b, md5_new, 16);
}

static void Benchmark20ByteSHA1Single(TestingB *b) {
    benchmark_hkdf(b, sha1_new, 20);
}

static void Benchmark32ByteSHA256Single(TestingB *b) {
    benchmark_hkdf(b, sha256_new, 32);
}

static void Benchmark64ByteSHA512Single(TestingB *b) {
    benchmark_hkdf(b, sha512_new, 64);
}

#define TESTS(X)                                                                       \
    X(TestHKDF)                                                                        \
    X(TestHKDFLimit)                                                                   \
    X(Benchmark16ByteMD5Single)                                                        \
    X(Benchmark20ByteSHA1Single)                                                       \
    X(Benchmark32ByteSHA256Single)                                                     \
    X(Benchmark64ByteSHA512Single)

TESTING_MAIN(TESTS)
