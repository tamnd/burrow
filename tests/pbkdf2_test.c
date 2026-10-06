/* Derived from Go's src/crypto/pbkdf2/pbkdf2_test.go.
 * Go source: go1.27.1.
 *
 * The vectors are RFC 6070's for SHA-1 and Go's for SHA-256, with the outputs
 * written as hex. TestPBKDF2ServiceIndicator is about Go's FIPS module, which
 * is not here, and TestWycheproof waits for the Wycheproof vectors, which are
 * fetched in CI rather than kept in the tree.
 *
 * Copyright 2012 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/crypto/pbkdf2.h"
#include "burrow/crypto/sha1.h"
#include "burrow/crypto/sha256.h"
#include "burrow/encoding/hex.h"

#include <stdint.h>
#include <string.h>

typedef struct Pbkdf2Vector {
    const char *password;
    Int password_len;
    const char *salt;
    Int salt_len;
    Int iter;
    const char *output;
} Pbkdf2Vector;

#define NELEM(x) (sizeof(x) / sizeof((x)[0]))

static const Pbkdf2Vector sha1_vectors[] = {
    {"password", 8, "salt", 4, 1, "0c60c80f961f0e71f3a9b524af6012062fe037a6"},
    {"password", 8, "salt", 4, 2, "ea6c014dc72d6f8ccd1ed92ace1d41f0d8de8957"},
    {"password", 8, "salt", 4, 4096, "4b007901b765489abead49d926f721d065a429c1"},
    {"passwordPASSWORDpassword", 24, "saltSALTsaltSALTsaltSALTsaltSALTsalt", 36, 4096,
     "3d2eec4fe41c849b80c8d83662c0e44a8b291a964cf2f07038"},
    {"pass\000word", 9, "sa\000lt", 5, 4096, "56fa6aa75548099dcc37d7f03425e0c3"},
};

static const Pbkdf2Vector sha256_vectors[] = {
    {"password", 8, "salt", 4, 1, "120fb6cffcf8b32c43e7225256c4f837a86548c9"},
    {"password", 8, "salt", 4, 2, "ae4d0c95af6b46d32d0adff928f06dd02a303f8e"},
    {"password", 8, "salt", 4, 4096, "c5e478d59288c841aa530db6845c4c8d962893a0"},
    {"passwordPASSWORDpassword", 24, "saltSALTsaltSALTsaltSALTsaltSALTsalt", 36, 4096,
     "348c89dbcbd32b2f32d814b8116e84cf2b17347ebc1800181c"},
    {"pass\000word", 9, "sa\000lt", 5, 4096, "89b69d0516f829893c696226650a8687"},
};

static Slice sb(const char *s, Int n) {
    return slice_from((void *)(uintptr_t)s, n, n, TYPE_BYTE);
}

static void test_hash(TestingT *t, HashNewFunc h, const char *hash_name,
                      const Pbkdf2Vector *vectors, size_t n) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < n; i++) {
        const Pbkdf2Vector *v = &vectors[i];
        Slice want = hex_decode_string(a, str_from_cstr(v->output), NULL);
        Error err;
        Slice o =
            pbkdf2_key(a, h, str_from_bytes((const Byte *)v->password, v->password_len),
                       sb(v->salt, v->salt_len), v->iter, want.len, &err);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "%s", error_text(err));
        if (o.len != want.len || memcmp(o.p, want.p, (size_t)want.len) != 0)
            testing_t_errorf_v(t, "%s %d: expected %x, got %x", hash_name, (Int)i,
                               str_from_bytes((const Byte *)want.p, want.len),
                               str_from_bytes((const Byte *)o.p, o.len));
    }
    arena_free(&ar);
}

static void TestWithHMACSHA1(TestingT *t) {
    test_hash(t, sha1_new, "SHA1", sha1_vectors, NELEM(sha1_vectors));
}

static void TestWithHMACSHA256(TestingT *t) {
    test_hash(t, sha256_new, "SHA256", sha256_vectors, NELEM(sha256_vectors));
}

static volatile Byte sink;

static void benchmark(TestingB *b, HashNewFunc h) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice password = slice_make(a, TYPE_BYTE, 0, 32);
    Slice salt = slice_make(a, TYPE_BYTE, 0, 32);
    for (Int i = 0; i < testing_b_n(b); i++) {
        if (i % 1024 == 0) {
            arena_reset(&ar);
            password = slice_make(a, TYPE_BYTE, 0, 32);
            salt = slice_make(a, TYPE_BYTE, 0, 32);
        }
        password =
            pbkdf2_key(a, h, str_from_bytes((const Byte *)password.p, password.len),
                       salt, 4096, 32, NULL);
    }
    sink = ((const Byte *)password.p)[0];
    arena_free(&ar);
}

static void BenchmarkHMACSHA1(TestingB *b) {
    benchmark(b, sha1_new);
}

static void BenchmarkHMACSHA256(TestingB *b) {
    benchmark(b, sha256_new);
}

static void want_error(TestingT *t, Error err, const char *want) {
    if (!str_eq(error_text(err), str_from_cstr(want)))
        testing_t_errorf_v(t, "error %q, want %q", error_text(err),
                           str_from_cstr(want));
}

static void TestMaxKeyLength(TestingT *t) {
    /* This error cannot happen where Int is 32 bits: the largest key_length
     * there times the hash length is always less than 1<<32-1 times it. */
    if (sizeof(Int) < 8) {
        testing_t_skip_v(t, "cannot be replicated on platforms where int is 31 bits");
        return;
    }
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err;
    int64_t key_size = INT64_MAX;
    pbkdf2_key(a, sha256_new, BURROW_S("password"), sb("salt", 4), 1, (Int)key_size,
               &err);
    if (BURROW_OK(err))
        testing_t_fatalf_v(
            t, "expected pbkdf2.Key to fail with extremely large keyLength");
    key_size = (int64_t)UINT32_MAX * (SHA256_SIZE + 1);
    pbkdf2_key(a, sha256_new, BURROW_S("password"), sb("salt", 4), 1, (Int)key_size,
               &err);
    if (BURROW_OK(err))
        testing_t_fatalf_v(
            t, "expected pbkdf2.Key to fail with extremely large keyLength");
    want_error(t, err, "pbkdf2: keyLength too long");
    arena_free(&ar);
}

static void TestZeroKeyLength(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err;
    pbkdf2_key(a, sha256_new, BURROW_S("password"), sb("salt", 4), 1, 0, &err);
    if (BURROW_OK(err))
        testing_t_fatalf_v(t, "expected pbkdf2.Key to fail with zero keyLength");
    pbkdf2_key(a, sha256_new, BURROW_S("password"), sb("salt", 4), 1, -1, &err);
    if (BURROW_OK(err))
        testing_t_fatalf_v(t, "expected pbkdf2.Key to fail with negative keyLength");
    want_error(t, err, "pbkdf2: keyLength must be larger than 0");
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestWithHMACSHA1)                                                                \
    X(TestWithHMACSHA256)                                                              \
    X(TestMaxKeyLength)                                                                \
    X(TestZeroKeyLength)                                                               \
    X(BenchmarkHMACSHA1)                                                               \
    X(BenchmarkHMACSHA256)

TESTING_MAIN(TESTS)
