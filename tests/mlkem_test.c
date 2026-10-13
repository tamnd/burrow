/* Derived from Go's src/crypto/mlkem/mlkem_test.go, mlkem_wycheproof_test.go and
 * src/crypto/internal/fips140/mlkem/field_test.go.
 * Go source: go1.27.1.
 *
 * The Wycheproof vectors Go fetches as a module are in mlkem_test_gen.h, which
 * tools/gen-mlkem-tests.sh writes. The keys and ciphertexts the tests only
 * compare against are there as their SHA-256.
 *
 * TestCompress and TestDecompress round with integers where Go uses big.Rat,
 * which comes to the same thing for these sizes. TestCAST is the known answer
 * test of Go's cast.go, which Go only runs in FIPS mode, and the tests after it
 * are burrow's, for the interfaces, the errors and the allocator.
 *
 * Copyright 2023 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/crypto.h"
#include "burrow/crypto/mlkem.h"
#include "burrow/crypto/mlkem/mlkemtest.h"
#include "burrow/crypto/rand.h"
#include "burrow/crypto/sha256.h"
#include "burrow/crypto/sha3.h"
#include "burrow/encoding/hex.h"
#include "burrow/mem/heap.h"
#include "burrow/slices.h"

#include "../src/crypto/mlkem_internal.h"

#include "mlkem_test_gen.h"

#include <stdint.h>
#include <string.h>

static Slice bs(void *p, Int n) {
    return slice_from(p, n, n, TYPE_BYTE);
}

static Slice unhex(Alloc *a, const char *s) {
    Error err = BURROW_NO_ERROR;
    Slice b = hex_decode_string(a, str_from_cstr(s), &err);
    if (BURROW_FAILED(err))
        panic_str(BURROW_S("invalid hex"));
    return b;
}

/* A hex string the generator has cut in two, as C99 can only take so much in
 * one literal. */
static Slice unhex_parts(Alloc *a, const char *const parts[2]) {
    if (parts[1] == NULL)
        return unhex(a, parts[0]);
    Slice b[2] = {unhex(a, parts[0]), unhex(a, parts[1])};
    return slices_concat(a, b, 2);
}

/* Whether the SHA-256 of b is the hex in want. */
static bool sha256_is(Slice b, const char *want) {
    static const char digits[] = "0123456789abcdef";
    Sha256Sum256Ret h = sha256_sum256(b);
    char got[2 * SHA256_SIZE + 1];
    for (int i = 0; i < SHA256_SIZE; i++) {
        got[2 * i] = digits[h.a[i] >> 4];
        got[2 * i + 1] = digits[h.a[i] & 15];
    }
    got[2 * SHA256_SIZE] = 0;
    return strcmp(got, want) == 0;
}

static void read_random(Slice b) {
    Error err = BURROW_NO_ERROR;
    crypto_rand_read(b, &err);
    if (BURROW_FAILED(err))
        panic_str(BURROW_S("crypto/rand failed"));
}

/* ------------------------------------------------------------------ field */

static void TestFieldReduce(TestingT *t) {
    for (uint32_t a = 0; a < 2 * MLKEM_Q * MLKEM_Q; a++) {
        MlkemFieldElement got = burrow__mlkem_field_reduce(a);
        MlkemFieldElement exp = (MlkemFieldElement)(a % MLKEM_Q);
        if (got != exp)
            testing_t_fatalf_v(t, "reduce(%d) = %d, expected %d", (Int)a, (int)got,
                               (int)exp);
    }
}

static void TestFieldAdd(TestingT *t) {
    for (int a = 0; a < MLKEM_Q; a++) {
        for (int b = 0; b < MLKEM_Q; b++) {
            int got =
                burrow__mlkem_field_add((MlkemFieldElement)a, (MlkemFieldElement)b);
            int exp = (a + b) % MLKEM_Q;
            if (got != exp)
                testing_t_fatalf_v(t, "%d + %d = %d, expected %d", a, b, got, exp);
        }
    }
}

static void TestFieldSub(TestingT *t) {
    for (int a = 0; a < MLKEM_Q; a++) {
        for (int b = 0; b < MLKEM_Q; b++) {
            int got =
                burrow__mlkem_field_sub((MlkemFieldElement)a, (MlkemFieldElement)b);
            int exp = (a - b + MLKEM_Q) % MLKEM_Q;
            if (got != exp)
                testing_t_fatalf_v(t, "%d - %d = %d, expected %d", a, b, got, exp);
        }
    }
}

static void TestFieldMul(TestingT *t) {
    for (int a = 0; a < MLKEM_Q; a++) {
        for (int b = 0; b < MLKEM_Q; b++) {
            int got =
                burrow__mlkem_field_mul((MlkemFieldElement)a, (MlkemFieldElement)b);
            int exp = (int)(((uint32_t)a * (uint32_t)b) % MLKEM_Q);
            if (got != exp)
                testing_t_fatalf_v(t, "%d * %d = %d, expected %d", a, b, got, exp);
        }
    }
}

static uint16_t min4(uint16_t a, uint16_t b, uint16_t c, uint16_t d) {
    uint16_t m = a;
    if (b < m)
        m = b;
    if (c < m)
        m = c;
    if (d < m)
        m = d;
    return m;
}

static void TestDecompressCompress(TestingT *t) {
    static const uint8_t bits_list[] = {1, 4, 10};
    for (size_t i = 0; i < sizeof bits_list; i++) {
        uint8_t bits = bits_list[i];
        for (int a = 0; a < 1 << bits; a++) {
            MlkemFieldElement f = burrow__mlkem_decompress((uint16_t)a, bits);
            if (f >= MLKEM_Q)
                testing_t_fatalf_v(t, "decompress(%d, %d) = %d >= q", a, (int)bits,
                                   (int)f);
            int got = burrow__mlkem_compress(f, bits);
            if (got != a)
                testing_t_fatalf_v(t, "compress(decompress(%d, %d), %d) = %d", a,
                                   (int)bits, (int)bits, got);
        }

        for (int a = 0; a < MLKEM_Q; a++) {
            uint16_t c = burrow__mlkem_compress((MlkemFieldElement)a, bits);
            if (c >= 1 << bits)
                testing_t_fatalf_v(t, "compress(%d, %d) = %d >= 2^bits", a, (int)bits,
                                   (int)c);
            uint16_t got = burrow__mlkem_decompress(c, bits);
            /* The differences wrap as Go's uint16 fieldElement does. */
            uint16_t diff =
                min4((uint16_t)(a - got), (uint16_t)(got - a),
                     (uint16_t)(a - got + MLKEM_Q), (uint16_t)(got - a + MLKEM_Q));
            int ceil = MLKEM_Q / (1 << bits);
            if (diff > ceil)
                testing_t_fatalf_v(
                    t,
                    "decompress(compress(%d, %d), %d) = %d (diff %d, max "
                    "diff %d)",
                    a, (int)bits, (int)bits, (int)got, (int)diff, ceil);
        }
    }
}

/* CompressRat: (2ᵈ / q) * x rounded, halves up, then reduced mod 2ᵈ. */
static uint16_t compress_rat(int x, int d) {
    uint64_t rounded = ((uint64_t)2 * ((uint64_t)1 << d) * (uint64_t)x + MLKEM_Q) /
                       ((uint64_t)2 * MLKEM_Q);
    return (uint16_t)(rounded % ((uint64_t)1 << d));
}

static void TestCompress(TestingT *t) {
    for (int d = 1; d < 12; d++) {
        for (int n = 0; n < MLKEM_Q; n++) {
            int expected = compress_rat(n, d);
            int result = burrow__mlkem_compress((MlkemFieldElement)n, (uint8_t)d);
            if (result != expected)
                testing_t_errorf_v(t, "compress(%d, %d): got %d, expected %d", n, d,
                                   result, expected);
        }
    }
}

/* DecompressRat: (q / 2ᵈ) * y rounded, halves up, then reduced mod q. */
static MlkemFieldElement decompress_rat(int y, int d) {
    uint64_t rounded = ((uint64_t)2 * MLKEM_Q * (uint64_t)y + ((uint64_t)1 << d)) /
                       ((uint64_t)1 << (d + 1));
    return (MlkemFieldElement)(rounded % MLKEM_Q);
}

static void TestDecompress(TestingT *t) {
    for (int d = 1; d < 12; d++) {
        for (int n = 0; n < 1 << d; n++) {
            int expected = decompress_rat(n, d);
            int result = burrow__mlkem_decompress((uint16_t)n, (uint8_t)d);
            if (result != expected)
                testing_t_errorf_v(t, "decompress(%d, %d): got %d, expected %d", n, d,
                                   result, expected);
        }
    }
}

static void random_ring_element(MlkemRing *r) {
    uint16_t b[MLKEM_N];
    read_random(bs(b, sizeof b));
    for (int i = 0; i < MLKEM_N; i++)
        r->c[i] = (MlkemFieldElement)(b[i] % MLKEM_Q);
}

static void TestEncodeDecode(TestingT *t) {
    MlkemRing f;
    random_ring_element(&f);
    Byte b[MLKEM_ENCODING_SIZE12];
    read_random(bs(b, sizeof b));

    /* Compare ringCompressAndEncode to ringCompressAndEncodeN. */
    Byte e1[MLKEM_ENCODING_SIZE12], e2[MLKEM_ENCODING_SIZE12];
    burrow__mlkem_ring_compress_and_encode(e1, &f, 10);
    burrow__mlkem_ring_compress_and_encode10(e2, &f);
    if (memcmp(e1, e2, MLKEM_ENCODING_SIZE10) != 0)
        testing_t_errorf_v(t, "ringCompressAndEncode != ringCompressAndEncode10");
    burrow__mlkem_ring_compress_and_encode(e1, &f, 4);
    burrow__mlkem_ring_compress_and_encode4(e2, &f);
    if (memcmp(e1, e2, MLKEM_ENCODING_SIZE4) != 0)
        testing_t_errorf_v(t, "ringCompressAndEncode != ringCompressAndEncode4");
    burrow__mlkem_ring_compress_and_encode(e1, &f, 1);
    burrow__mlkem_ring_compress_and_encode1(e2, &f);
    if (memcmp(e1, e2, MLKEM_ENCODING_SIZE1) != 0)
        testing_t_errorf_v(t, "ringCompressAndEncode != ringCompressAndEncode1");

    /* Compare ringDecodeAndDecompress to ringDecodeAndDecompressN. */
    MlkemRing g1, g2;
    burrow__mlkem_ring_decode_and_decompress(&g1, b, 10);
    burrow__mlkem_ring_decode_and_decompress10(&g2, b);
    if (memcmp(&g1, &g2, sizeof g1) != 0)
        testing_t_errorf_v(t, "ringDecodeAndDecompress != ringDecodeAndDecompress10");
    burrow__mlkem_ring_decode_and_decompress(&g1, b, 4);
    burrow__mlkem_ring_decode_and_decompress4(&g2, b);
    if (memcmp(&g1, &g2, sizeof g1) != 0)
        testing_t_errorf_v(t, "ringDecodeAndDecompress != ringDecodeAndDecompress4");
    burrow__mlkem_ring_decode_and_decompress(&g1, b, 1);
    burrow__mlkem_ring_decode_and_decompress1(&g2, b);
    if (memcmp(&g1, &g2, sizeof g1) != 0)
        testing_t_errorf_v(t, "ringDecodeAndDecompress != ringDecodeAndDecompress1");

    /* Round-trip ringCompressAndEncode and ringDecodeAndDecompress. */
    for (int d = 1; d < 12; d++) {
        int encoding_size = d * MLKEM_N / 8;
        MlkemRing g;
        Byte out[MLKEM_ENCODING_SIZE12];
        burrow__mlkem_ring_decode_and_decompress(&g, b, (uint8_t)d);
        burrow__mlkem_ring_compress_and_encode(out, &g, (uint8_t)d);
        if (memcmp(out, b, (size_t)encoding_size) != 0)
            testing_t_errorf_v(t, "roundtrip failed for d = %d", d);
    }

    /* Round-trip ringCompressAndEncodeN and ringDecodeAndDecompressN. */
    MlkemRing g;
    Byte out[MLKEM_ENCODING_SIZE12];
    burrow__mlkem_ring_decode_and_decompress10(&g, b);
    burrow__mlkem_ring_compress_and_encode10(out, &g);
    if (memcmp(out, b, MLKEM_ENCODING_SIZE10) != 0)
        testing_t_errorf_v(t, "roundtrip failed for specialized 10");
    burrow__mlkem_ring_decode_and_decompress4(&g, b);
    burrow__mlkem_ring_compress_and_encode4(out, &g);
    if (memcmp(out, b, MLKEM_ENCODING_SIZE4) != 0)
        testing_t_errorf_v(t, "roundtrip failed for specialized 4");
    burrow__mlkem_ring_decode_and_decompress1(&g, b);
    burrow__mlkem_ring_compress_and_encode1(out, &g);
    if (memcmp(out, b, MLKEM_ENCODING_SIZE1) != 0)
        testing_t_errorf_v(t, "roundtrip failed for specialized 1");
}

static int bit_rev7(int n) {
    int r = 0;
    for (int i = 0; i < 7; i++)
        r |= ((n >> i) & 1) << (6 - i);
    return r;
}

/* ζᵉ mod q, with ζ = 17. */
static int zeta_pow(int e) {
    uint32_t r = 1;
    for (int i = 0; i < e; i++)
        r = r * 17 % MLKEM_Q;
    return (int)r;
}

static void TestZetas(TestingT *t) {
    for (int k = 0; k < 128; k++) {
        int exp = zeta_pow(bit_rev7(k));
        if (burrow__mlkem_zetas[k] != exp)
            testing_t_errorf_v(t, "zetas[%d] = %d, expected %d", k,
                               (int)burrow__mlkem_zetas[k], exp);
    }
}

static void TestGammas(TestingT *t) {
    for (int k = 0; k < 128; k++) {
        int exp = zeta_pow(2 * bit_rev7(k) + 1);
        if (burrow__mlkem_gammas[k] != exp)
            testing_t_errorf_v(t, "gammas[%d] = %d, expected %d", k,
                               (int)burrow__mlkem_gammas[k], exp);
    }
}

/* ------------------------------------------------------------------- KEM */

/* The two parameter sets behind one set of function pointers, as Go's tests
 * take them through generic functions. */
typedef struct Kem {
    const char *name;
    Int ciphertext_size, encapsulation_key_size;
    void *(*generate_key)(Alloc *a, Error *err);
    void *(*new_dk)(Alloc *a, Slice seed, Error *err);
    void (*free_dk)(void *dk);
    Slice (*dk_bytes)(const void *dk, Alloc *a);
    Slice (*decapsulate)(const void *dk, Alloc *a, Slice c, Error *err);
    const void *(*ek_of)(const void *dk);
    void *(*new_ek)(Alloc *a, Slice b, Error *err);
    void (*free_ek)(void *ek);
    Slice (*ek_bytes)(const void *ek, Alloc *a);
    Slice (*encapsulate)(const void *ek, Alloc *a, Slice *c);
    Slice (*encapsulate_random)(const void *ek, Alloc *a, Slice m, Slice *c,
                                Error *err);
    Slice (*expanded_bytes)(const void *dk, Alloc *a);
    void *(*new_expanded)(Alloc *a, Slice b, Error *err);
} Kem;

#define KEM(N)                                                                         \
    static void *kem##N##_generate_key(Alloc *a, Error *err) {                         \
        return mlkem_generate_key##N(a, err);                                          \
    }                                                                                  \
    static void *kem##N##_new_dk(Alloc *a, Slice seed, Error *err) {                   \
        return mlkem_new_decapsulation_key##N(a, seed, err);                           \
    }                                                                                  \
    static void kem##N##_free_dk(void *dk) {                                           \
        mlkem_decapsulation_key##N##_free(dk);                                         \
    }                                                                                  \
    static Slice kem##N##_dk_bytes(const void *dk, Alloc *a) {                         \
        return mlkem_decapsulation_key##N##_bytes(dk, a);                              \
    }                                                                                  \
    static Slice kem##N##_decapsulate(const void *dk, Alloc *a, Slice c, Error *err) { \
        return mlkem_decapsulation_key##N##_decapsulate(dk, a, c, err);                \
    }                                                                                  \
    static const void *kem##N##_ek_of(const void *dk) {                                \
        return mlkem_decapsulation_key##N##_encapsulation_key(dk);                     \
    }                                                                                  \
    static void *kem##N##_new_ek(Alloc *a, Slice b, Error *err) {                      \
        return mlkem_new_encapsulation_key##N(a, b, err);                              \
    }                                                                                  \
    static void kem##N##_free_ek(void *ek) {                                           \
        mlkem_encapsulation_key##N##_free(ek);                                         \
    }                                                                                  \
    static Slice kem##N##_ek_bytes(const void *ek, Alloc *a) {                         \
        return mlkem_encapsulation_key##N##_bytes(ek, a);                              \
    }                                                                                  \
    static Slice kem##N##_encapsulate(const void *ek, Alloc *a, Slice *c) {            \
        return mlkem_encapsulation_key##N##_encapsulate(ek, a, c);                     \
    }                                                                                  \
    static Slice kem##N##_encapsulate_random(const void *ek, Alloc *a, Slice m,        \
                                             Slice *c, Error *err) {                   \
        return mlkemtest_encapsulate##N(ek, a, m, c, err);                             \
    }                                                                                  \
    static Slice kem##N##_expanded_bytes(const void *dk, Alloc *a) {                   \
        return burrow__mlkem_testing_only_expanded_bytes##N(dk, a);                    \
    }                                                                                  \
    static void *kem##N##_new_expanded(Alloc *a, Slice b, Error *err) {                \
        return burrow__mlkem_testing_only_new_decapsulation_key##N(a, b, err);         \
    }                                                                                  \
    static const Kem kem##N = {                                                        \
        #N,                                                                            \
        MLKEM_CIPHERTEXT_SIZE##N,                                                      \
        MLKEM_ENCAPSULATION_KEY_SIZE##N,                                               \
        kem##N##_generate_key,                                                         \
        kem##N##_new_dk,                                                               \
        kem##N##_free_dk,                                                              \
        kem##N##_dk_bytes,                                                             \
        kem##N##_decapsulate,                                                          \
        kem##N##_ek_of,                                                                \
        kem##N##_new_ek,                                                               \
        kem##N##_free_ek,                                                              \
        kem##N##_ek_bytes,                                                             \
        kem##N##_encapsulate,                                                          \
        kem##N##_encapsulate_random,                                                   \
        kem##N##_expanded_bytes,                                                       \
        kem##N##_new_expanded,                                                         \
    };

KEM(768)
KEM(1024)

static const Kem *kem_for(int params) {
    return params == 768 ? &kem768 : &kem1024;
}

static void round_trip(void *env, TestingT *t) {
    const Kem *k = env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;

    void *dk = k->generate_key(a, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    const void *ek = k->ek_of(dk);
    Slice c;
    Slice ke = k->encapsulate(ek, a, &c);
    Slice kd = k->decapsulate(dk, a, c, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    if (!bytes_equal(ke, kd))
        testing_t_fail(t);

    void *ek1 = k->new_ek(a, k->ek_bytes(ek, a), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    if (!bytes_equal(k->ek_bytes(ek, a), k->ek_bytes(ek1, a)))
        testing_t_fail(t);
    void *dk1 = k->new_dk(a, k->dk_bytes(dk, a), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    if (!bytes_equal(k->dk_bytes(dk, a), k->dk_bytes(dk1, a)))
        testing_t_fail(t);
    Slice c1;
    Slice ke1 = k->encapsulate(ek1, a, &c1);
    Slice kd1 = k->decapsulate(dk1, a, c1, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    if (!bytes_equal(ke1, kd1))
        testing_t_fail(t);

    void *dk2 = k->generate_key(a, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    if (bytes_equal(k->ek_bytes(k->ek_of(dk), a), k->ek_bytes(k->ek_of(dk2), a)))
        testing_t_fail(t);
    if (bytes_equal(k->dk_bytes(dk, a), k->dk_bytes(dk2, a)))
        testing_t_fail(t);

    Slice c2;
    Slice ke2 = k->encapsulate(k->ek_of(dk), a, &c2);
    if (bytes_equal(c, c2))
        testing_t_fail(t);
    if (bytes_equal(ke, ke2))
        testing_t_fail(t);
    arena_free(&ar);
}

static void TestRoundTrip(TestingT *t) {
    testing_t_run(t, BURROW_S("768"),
                  BURROW_FN(TestingTFunc, round_trip, (void *)(uintptr_t)&kem768));
    testing_t_run(t, BURROW_S("1024"),
                  BURROW_FN(TestingTFunc, round_trip, (void *)(uintptr_t)&kem1024));
}

/* b with n more zero bytes on the end, from a. */
static Slice append_zeros(Alloc *a, Slice b, Int n) {
    Slice s = slice_make(a, TYPE_BYTE, b.len + n, b.len + n);
    memcpy(s.p, b.p, (size_t)b.len);
    memset((Byte *)s.p + b.len, 0, (size_t)n);
    return s;
}

static void bad_lengths(void *env, TestingT *t) {
    const Kem *k = env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;

    void *dk = k->generate_key(a, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    Slice dk_bytes = k->dk_bytes(dk, a);
    const void *ek = k->ek_of(dk);
    Slice ek_bytes = k->ek_bytes(ek, a);
    Slice c;
    k->encapsulate(ek, a, &c);

    for (Int i = 0; i < dk_bytes.len - 1; i++) {
        if (k->new_dk(a, slice_sub(dk_bytes, 0, i), &err) != NULL ||
            !BURROW_FAILED(err))
            testing_t_errorf_v(t, "expected error for dk length %d", i);
    }
    for (Int i = 1; i <= 100; i++) {
        if (k->new_dk(a, append_zeros(a, dk_bytes, i), &err) != NULL ||
            !BURROW_FAILED(err))
            testing_t_errorf_v(t, "expected error for dk length %d", dk_bytes.len + i);
    }

    for (Int i = 0; i < ek_bytes.len - 1; i++) {
        if (k->new_ek(a, slice_sub(ek_bytes, 0, i), &err) != NULL ||
            !BURROW_FAILED(err))
            testing_t_errorf_v(t, "expected error for ek length %d", i);
    }
    for (Int i = 1; i <= 100; i++) {
        if (k->new_ek(a, append_zeros(a, ek_bytes, i), &err) != NULL ||
            !BURROW_FAILED(err))
            testing_t_errorf_v(t, "expected error for ek length %d", ek_bytes.len + i);
    }

    for (Int i = 0; i < c.len - 1; i++) {
        k->decapsulate(dk, a, slice_sub(c, 0, i), &err);
        if (!BURROW_FAILED(err))
            testing_t_errorf_v(t, "expected error for c length %d", i);
    }
    for (Int i = 1; i <= 100; i++) {
        k->decapsulate(dk, a, append_zeros(a, c, i), &err);
        if (!BURROW_FAILED(err))
            testing_t_errorf_v(t, "expected error for c length %d", c.len + i);
    }
    arena_free(&ar);
}

static void TestBadLengths(TestingT *t) {
    testing_t_run(t, BURROW_S("768"),
                  BURROW_FN(TestingTFunc, bad_lengths, (void *)(uintptr_t)&kem768));
    testing_t_run(t, BURROW_S("1024"),
                  BURROW_FN(TestingTFunc, bad_lengths, (void *)(uintptr_t)&kem1024));
}

/* TestAccumulated accumulates 10k (or 100) random vectors and checks the hash
 * of the result, to avoid checking in 150MB of test vectors. Go's -million flag
 * is not here. */
static void TestAccumulated(TestingT *t) {
    Int n = 10000;
    const char *expected =
        "8a518cc63da366322a8e7a818c7a0d63483cb3528d34a4cf42f35d5ad73f22fc";
    if (testing_short()) {
        n = 100;
        expected = "1114b1b6699ed191734fa339376afa7e285c9e6acf6ff0177d346696ce564415";
    }

    Arena keep, ar;
    arena_init(&keep, NULL, 0);
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Sha3SHAKE *s = sha3_new_shake128(arena_allocator(&keep));
    Sha3SHAKE *o = sha3_new_shake128(arena_allocator(&keep));
    Byte seed[MLKEM_SEED_SIZE], msg[32], ct1[MLKEM_CIPHERTEXT_SIZE768];

    for (Int i = 0; i < n; i++) {
        Error err = BURROW_NO_ERROR;
        sha3_shake_read(s, bs(seed, sizeof seed), NULL);
        MlkemDecapsulationKey768 *dk =
            mlkem_new_decapsulation_key768(a, bs(seed, sizeof seed), &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "%s", error_text(err));
        const MlkemEncapsulationKey768 *ek =
            mlkem_decapsulation_key768_encapsulation_key(dk);
        sha3_shake_write(o, mlkem_encapsulation_key768_bytes(ek, a), NULL);

        sha3_shake_read(s, bs(msg, sizeof msg), NULL);
        Slice ct;
        Slice k = mlkemtest_encapsulate768(ek, a, bs(msg, sizeof msg), &ct, &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "%s", error_text(err));
        sha3_shake_write(o, ct, NULL);
        sha3_shake_write(o, k, NULL);

        Slice kk = mlkem_decapsulation_key768_decapsulate(dk, a, ct, &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "%s", error_text(err));
        if (!bytes_equal(kk, k))
            testing_t_errorf_v(t, "k: got %s, expected %s", hex_encode_to_string(a, kk),
                               hex_encode_to_string(a, k));

        sha3_shake_read(s, bs(ct1, sizeof ct1), NULL);
        Slice k1 =
            mlkem_decapsulation_key768_decapsulate(dk, a, bs(ct1, sizeof ct1), &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "%s", error_text(err));
        sha3_shake_write(o, k1, NULL);
        arena_reset(&ar);
    }

    /* o.Sum(nil), 32 bytes of output for SHAKE128. */
    Byte sum[32];
    sha3_shake_read(o, bs(sum, sizeof sum), NULL);
    Str got = hex_encode_to_string(a, bs(sum, sizeof sum));
    if (!str_eq(got, str_from_cstr(expected)))
        testing_t_errorf_v(t, "got %s, expected %s", got, expected);
    arena_free(&ar);
    arena_free(&keep);
}

/* Test that the constants from the public API match the values the internal
 * sizes give. */
static void TestConstantSizes(TestingT *t) {
    if (MLKEM_SHARED_KEY_SIZE != 32)
        testing_t_errorf_v(t, "SharedKeySize mismatch: got %d, want %d",
                           MLKEM_SHARED_KEY_SIZE, 32);
    if (MLKEM_SEED_SIZE != 64)
        testing_t_errorf_v(t, "SeedSize mismatch: got %d, want %d", MLKEM_SEED_SIZE,
                           64);
    int ct768 = 3 * MLKEM_ENCODING_SIZE10 + MLKEM_ENCODING_SIZE4;
    if (MLKEM_CIPHERTEXT_SIZE768 != ct768)
        testing_t_errorf_v(t, "CiphertextSize768 mismatch: got %d, want %d",
                           MLKEM_CIPHERTEXT_SIZE768, ct768);
    int ek768 = 3 * MLKEM_ENCODING_SIZE12 + 32;
    if (MLKEM_ENCAPSULATION_KEY_SIZE768 != ek768)
        testing_t_errorf_v(t, "EncapsulationKeySize768 mismatch: got %d, want %d",
                           MLKEM_ENCAPSULATION_KEY_SIZE768, ek768);
    int ct1024 = 4 * MLKEM_ENCODING_SIZE11 + MLKEM_ENCODING_SIZE5;
    if (MLKEM_CIPHERTEXT_SIZE1024 != ct1024)
        testing_t_errorf_v(t, "CiphertextSize1024 mismatch: got %d, want %d",
                           MLKEM_CIPHERTEXT_SIZE1024, ct1024);
    int ek1024 = 4 * MLKEM_ENCODING_SIZE12 + 32;
    if (MLKEM_ENCAPSULATION_KEY_SIZE1024 != ek1024)
        testing_t_errorf_v(t, "EncapsulationKeySize1024 mismatch: got %d, want %d",
                           MLKEM_ENCAPSULATION_KEY_SIZE1024, ek1024);
}

/* ------------------------------------------------------------ Wycheproof */

static void TestKeyGenWycheproof(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof gen_mlkem_keygen / sizeof gen_mlkem_keygen[0]; i++) {
        const GenMlkemKeyGen *tv = &gen_mlkem_keygen[i];
        const Kem *k = kem_for(tv->params);
        Error err = BURROW_NO_ERROR;
        Slice seed = unhex(a, tv->seed);
        void *dk = k->new_dk(a, seed, &err);
        if (BURROW_FAILED(err)) {
            if (tv->valid)
                testing_t_fatalf_v(t, "%d/%d: NewDecapsulationKey%s: %s", tv->params,
                                   tv->tc_id, k->name, error_text(err));
            arena_reset(&ar);
            continue;
        }
        if (!bytes_equal(k->dk_bytes(dk, a), seed))
            testing_t_errorf_v(t, "%d/%d: decapsulation key seed roundtrip mismatch",
                               tv->params, tv->tc_id);
        const void *ek = k->ek_of(dk);
        Slice ek_bytes = k->ek_bytes(ek, a);
        if (!sha256_is(ek_bytes, tv->ek_sha256))
            testing_t_errorf_v(t, "%d/%d: encapsulation key mismatch", tv->params,
                               tv->tc_id);
        if (!sha256_is(k->expanded_bytes(dk, a), tv->dk_sha256))
            testing_t_errorf_v(t, "%d/%d: expanded decapsulation key mismatch",
                               tv->params, tv->tc_id);
        void *ek2 = k->new_ek(a, ek_bytes, &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "%d/%d: NewEncapsulationKey%s: %s", tv->params,
                               tv->tc_id, k->name, error_text(err));
        if (!bytes_equal(k->ek_bytes(ek2, a), ek_bytes))
            testing_t_errorf_v(t, "%d/%d: encapsulation key roundtrip mismatch",
                               tv->params, tv->tc_id);
        Slice c;
        Slice k1 = k->encapsulate(ek, a, &c);
        Slice k2 = k->decapsulate(dk, a, c, &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "%d/%d: Decapsulate: %s", tv->params, tv->tc_id,
                               error_text(err));
        if (!bytes_equal(k1, k2))
            testing_t_errorf_v(t, "%d/%d: encaps/decaps roundtrip key mismatch",
                               tv->params, tv->tc_id);
        arena_reset(&ar);
    }
    arena_free(&ar);
}

static void TestMLKEMEncapsWycheproof(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof gen_mlkem_encaps / sizeof gen_mlkem_encaps[0]; i++) {
        const GenMlkemEncaps *tv = &gen_mlkem_encaps[i];
        const Kem *k = kem_for(tv->params);
        Error err = BURROW_NO_ERROR;
        Slice ek_bytes = unhex_parts(a, tv->ek);
        Slice m = unhex(a, tv->m);
        void *ek = k->new_ek(a, ek_bytes, &err);
        if (BURROW_FAILED(err)) {
            if (tv->pass)
                testing_t_fatalf_v(t, "%d/%d: NewEncapsulationKey%s: %s", tv->params,
                                   tv->tc_id, k->name, error_text(err));
            arena_reset(&ar);
            continue;
        }
        if (!bytes_equal(k->ek_bytes(ek, a), ek_bytes))
            testing_t_errorf_v(t, "%d/%d: encapsulation key roundtrip mismatch",
                               tv->params, tv->tc_id);
        Slice c;
        Slice key = k->encapsulate_random(ek, a, m, &c, &err);
        if (BURROW_FAILED(err)) {
            if (tv->pass)
                testing_t_fatalf_v(t, "%d/%d: Encapsulate%s: %s", tv->params, tv->tc_id,
                                   k->name, error_text(err));
            arena_reset(&ar);
            continue;
        }
        if (!tv->pass) {
            testing_t_errorf_v(t, "%d/%d: Encapsulate%s unexpectedly succeeded",
                               tv->params, tv->tc_id, k->name);
            arena_reset(&ar);
            continue;
        }
        if (!sha256_is(c, tv->c_sha256))
            testing_t_errorf_v(t, "%d/%d: ciphertext mismatch", tv->params, tv->tc_id);
        if (!bytes_equal(key, unhex(a, tv->k)))
            testing_t_errorf_v(t, "%d/%d: shared key mismatch", tv->params, tv->tc_id);
        arena_reset(&ar);
    }
    arena_free(&ar);
}

static void TestMLKEMDecapsWycheproof(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof gen_mlkem_decaps / sizeof gen_mlkem_decaps[0]; i++) {
        const GenMlkemDecaps *tv = &gen_mlkem_decaps[i];
        const Kem *k = kem_for(tv->params);
        Error err = BURROW_NO_ERROR;
        Slice seed = unhex(a, tv->seed);
        Slice ciphertext = unhex_parts(a, tv->c);
        void *dk = k->new_dk(a, seed, &err);
        if (BURROW_FAILED(err)) {
            if (tv->pass)
                testing_t_fatalf_v(t, "%d/%d: NewDecapsulationKey%s: %s", tv->params,
                                   tv->tc_id, k->name, error_text(err));
            arena_reset(&ar);
            continue;
        }
        if (!bytes_equal(k->dk_bytes(dk, a), seed))
            testing_t_errorf_v(t, "%d/%d: decapsulation key seed roundtrip mismatch",
                               tv->params, tv->tc_id);
        if (tv->ek_sha256[0] != 0 &&
            !sha256_is(k->ek_bytes(k->ek_of(dk), a), tv->ek_sha256))
            testing_t_errorf_v(t, "%d/%d: encapsulation key mismatch", tv->params,
                               tv->tc_id);
        Slice key = k->decapsulate(dk, a, ciphertext, &err);
        if (BURROW_FAILED(err)) {
            if (tv->pass)
                testing_t_fatalf_v(t, "%d/%d: Decapsulate: %s", tv->params, tv->tc_id,
                                   error_text(err));
            arena_reset(&ar);
            continue;
        }
        if (tv->pass) {
            if (!bytes_equal(key, unhex(a, tv->k)))
                testing_t_errorf_v(t, "%d/%d: shared key mismatch: got %s, want %s",
                                   tv->params, tv->tc_id, hex_encode_to_string(a, key),
                                   tv->k);
            Slice c_fresh;
            Slice k_fresh = k->encapsulate(k->ek_of(dk), a, &c_fresh);
            Slice k_rt = k->decapsulate(dk, a, c_fresh, &err);
            if (BURROW_FAILED(err))
                testing_t_fatalf_v(t, "%d/%d: Decapsulate of fresh ciphertext: %s",
                                   tv->params, tv->tc_id, error_text(err));
            if (!bytes_equal(k_fresh, k_rt))
                testing_t_errorf_v(t, "%d/%d: encaps/decaps roundtrip key mismatch",
                                   tv->params, tv->tc_id);
        }
        arena_reset(&ar);
    }
    arena_free(&ar);
}

static void TestMLKEMSemiExpandedDecapsWycheproof(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof gen_mlkem_semi / sizeof gen_mlkem_semi[0]; i++) {
        const GenMlkemSemi *tv = &gen_mlkem_semi[i];
        const Kem *k = kem_for(tv->params);
        Error err = BURROW_NO_ERROR;
        Slice dk_bytes = unhex_parts(a, tv->dk);
        Slice ciphertext = unhex_parts(a, tv->c);
        void *dk = k->new_expanded(a, dk_bytes, &err);
        if (BURROW_FAILED(err)) {
            if (tv->pass)
                testing_t_fatalf_v(t, "%d/%d: TestingOnlyNewDecapsulationKey%s: %s",
                                   tv->params, tv->tc_id, k->name, error_text(err));
            arena_reset(&ar);
            continue;
        }
        if (!bytes_equal(k->expanded_bytes(dk, a), dk_bytes))
            testing_t_errorf_v(t,
                               "%d/%d: expanded decapsulation key roundtrip mismatch",
                               tv->params, tv->tc_id);
        if (!sha256_is(k->ek_bytes(k->ek_of(dk), a), tv->ek_sha256))
            testing_t_errorf_v(t, "%d/%d: encapsulation key mismatch", tv->params,
                               tv->tc_id);
        Slice key = k->decapsulate(dk, a, ciphertext, &err);
        if (BURROW_FAILED(err)) {
            if (tv->pass)
                testing_t_fatalf_v(t, "%d/%d: Decapsulate: %s", tv->params, tv->tc_id,
                                   error_text(err));
            arena_reset(&ar);
            continue;
        }
        if (!tv->pass) {
            testing_t_errorf_v(t, "%d/%d: Decapsulate unexpectedly succeeded",
                               tv->params, tv->tc_id);
            arena_reset(&ar);
            continue;
        }
        if (!tv->has_k)
            testing_t_fatalf_v(t,
                               "%d/%d: Decapsulate succeeded but test vector has no "
                               "expected K",
                               tv->params, tv->tc_id);
        if (!bytes_equal(key, unhex(a, tv->k)))
            testing_t_errorf_v(t, "%d/%d: shared key mismatch:\n got: %s\nwant: %s",
                               tv->params, tv->tc_id, hex_encode_to_string(a, key),
                               tv->k);
        arena_reset(&ar);
    }
    arena_free(&ar);
}

/* ---------------------------------------------------------------- burrow */

/* The ML-KEM-768 known answer test of Go's cast.go. */
static void TestCAST(TestingT *t) {
    static const Byte want[32] = {
        0x55, 0x01, 0xfc, 0x52, 0x3b, 0x74, 0x5f, 0x41, 0x76, 0x2a, 0x18,
        0x8d, 0xe4, 0x4a, 0x59, 0xb9, 0x20, 0xf4, 0x30, 0x14, 0x62, 0x04,
        0xee, 0x4e, 0x79, 0x37, 0x32, 0x39, 0x6d, 0xf7, 0xaa, 0x48,
    };
    Byte d[32], z[32], m[32];
    for (int i = 0; i < 32; i++) {
        d[i] = (Byte)(0x01 + i);
        z[i] = (Byte)(0x21 + i);
        m[i] = (Byte)(0x41 + i);
    }
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    MlkemDecapsulationKey768 *dk = burrow__mlkem_generate_key_internal768(a, d, z);
    Slice c;
    Slice ke = burrow__mlkem_encapsulate_internal768(
        mlkem_decapsulation_key768_encapsulation_key(dk), a, m, &c);
    Error err = BURROW_NO_ERROR;
    Slice kd = mlkem_decapsulation_key768_decapsulate(dk, a, c, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    Slice k = bs((void *)(uintptr_t)want, 32);
    if (!bytes_equal(ke, k) || !bytes_equal(kd, k))
        testing_t_errorf_v(t, "unexpected result: %s, %s", hex_encode_to_string(a, ke),
                           hex_encode_to_string(a, kd));
    arena_free(&ar);
}

static void TestInterfaces(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;

    MlkemDecapsulationKey768 *dk = mlkem_generate_key768(a, &err);
    CryptoDecapsulator d = mlkem_decapsulation_key768_as_decapsulator(dk);
    if (d.vt->self_type != TYPE_MLKEM_DECAPSULATION_KEY768)
        testing_t_errorf_v(t, "Decapsulator type is %s", d.vt->self_type->name);
    CryptoEncapsulator e = crypto_decapsulator_encapsulator(d);
    if (e.vt->self_type != TYPE_MLKEM_ENCAPSULATION_KEY768)
        testing_t_errorf_v(t, "Encapsulator type is %s", e.vt->self_type->name);
    if (e.data != (void *)(uintptr_t)mlkem_decapsulation_key768_encapsulation_key(dk))
        testing_t_errorf_v(t, "Encapsulator is not the key's own");
    CryptoEncapsulator e2 = mlkem_decapsulation_key768_encapsulator(dk);
    if (e2.vt != e.vt || e2.data != e.data)
        testing_t_errorf_v(t,
                           "Encapsulator and the Decapsulator's Encapsulator differ");
    if (!bytes_equal(crypto_encapsulator_bytes(e, a),
                     mlkem_encapsulation_key768_bytes(
                         mlkem_decapsulation_key768_encapsulation_key(dk), a)))
        testing_t_errorf_v(t, "Encapsulator Bytes mismatch");
    CryptoEncapsulateResult r = crypto_encapsulator_encapsulate(e, a);
    if (r.ciphertext.len != MLKEM_CIPHERTEXT_SIZE768 ||
        r.shared_key.len != MLKEM_SHARED_KEY_SIZE)
        testing_t_fatalf_v(t, "Encapsulate gave %d and %d bytes", r.ciphertext.len,
                           r.shared_key.len);
    Slice k = crypto_decapsulator_decapsulate(d, a, r.ciphertext, &err);
    if (BURROW_FAILED(err) || !bytes_equal(k, r.shared_key))
        testing_t_errorf_v(t, "Decapsulator does not get the shared key back");

    MlkemDecapsulationKey1024 *dk1024 = mlkem_generate_key1024(a, &err);
    MlkemEncapsulationKey1024 *ek1024 = mlkem_new_encapsulation_key1024(
        a,
        mlkem_encapsulation_key1024_bytes(
            mlkem_decapsulation_key1024_encapsulation_key(dk1024), a),
        &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    e = mlkem_encapsulation_key1024_as_encapsulator(ek1024);
    if (e.vt->self_type != TYPE_MLKEM_ENCAPSULATION_KEY1024)
        testing_t_errorf_v(t, "Encapsulator type is %s", e.vt->self_type->name);
    r = crypto_encapsulator_encapsulate(e, a);
    d = mlkem_decapsulation_key1024_as_decapsulator(dk1024);
    if (d.vt->self_type != TYPE_MLKEM_DECAPSULATION_KEY1024)
        testing_t_errorf_v(t, "Decapsulator type is %s", d.vt->self_type->name);
    k = crypto_decapsulator_decapsulate(d, a, r.ciphertext, &err);
    if (BURROW_FAILED(err) || !bytes_equal(k, r.shared_key))
        testing_t_errorf_v(t, "Decapsulator does not get the shared key back");
    arena_free(&ar);
}

static void want_error(TestingT *t, const char *what, Error err, const char *text) {
    testing_t_helper(t);
    if (!BURROW_FAILED(err))
        testing_t_errorf_v(t, "%s: no error, want %s", what, text);
    else if (!str_eq(error_text(err), str_from_cstr(text)))
        testing_t_errorf_v(t, "%s: error %s, want %s", what, error_text(err), text);
}

static void TestErrors(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    Byte zeros[MLKEM_SEED_SIZE + 1] = {0};

    mlkem_new_decapsulation_key768(a, bs(zeros, 63), &err);
    want_error(t, "NewDecapsulationKey768", err, "mlkem: invalid seed length");
    mlkem_new_decapsulation_key1024(a, bs(zeros, 65), &err);
    want_error(t, "NewDecapsulationKey1024", err, "mlkem: invalid seed length");

    MlkemDecapsulationKey768 *dk =
        mlkem_new_decapsulation_key768(a, bs(zeros, 64), &err);
    const MlkemEncapsulationKey768 *ek =
        mlkem_decapsulation_key768_encapsulation_key(dk);
    Slice ek_bytes = mlkem_encapsulation_key768_bytes(ek, a);
    mlkem_new_encapsulation_key768(a, slice_sub(ek_bytes, 1, ek_bytes.len), &err);
    want_error(t, "NewEncapsulationKey768", err,
               "mlkem: invalid encapsulation key length");
    /* A coefficient of 4095, which is not reduced. */
    Slice bad = slices_clone(a, ek_bytes);
    ((Byte *)bad.p)[0] = 0xff;
    ((Byte *)bad.p)[1] |= 0x0f;
    mlkem_new_encapsulation_key768(a, bad, &err);
    want_error(t, "NewEncapsulationKey768", err, "mlkem: invalid polynomial encoding");

    mlkem_decapsulation_key768_decapsulate(dk, a, bs(zeros, 64), &err);
    want_error(t, "Decapsulate", err, "mlkem: invalid ciphertext length");

    Slice c;
    Slice k = mlkemtest_encapsulate768(ek, a, bs(zeros, 31), &c, &err);
    want_error(t, "mlkemtest.Encapsulate768", err,
               "mlkemtest: Encapsulate768: random must be 32 bytes");
    if (k.p != NULL || c.p != NULL)
        testing_t_errorf_v(t, "mlkemtest.Encapsulate768 gave bytes with its error");
    MlkemDecapsulationKey1024 *dk1024 =
        mlkem_new_decapsulation_key1024(a, bs(zeros, 64), &err);
    mlkemtest_encapsulate1024(mlkem_decapsulation_key1024_encapsulation_key(dk1024), a,
                              bs(zeros, 33), &c, &err);
    want_error(t, "mlkemtest.Encapsulate1024", err,
               "mlkemtest: Encapsulate1024: random must be 32 bytes");

    Slice expanded = burrow__mlkem_testing_only_expanded_bytes768(dk, a);
    if (expanded.len != MLKEM_DECAPSULATION_KEY_SIZE768)
        testing_t_fatalf_v(t, "expanded key is %d bytes", expanded.len);
    burrow__mlkem_testing_only_new_decapsulation_key768(
        a, slice_sub(expanded, 0, expanded.len - 1), &err);
    want_error(t, "TestingOnlyNewDecapsulationKey768", err,
               "mlkem: invalid NIST decapsulation key length");
    bad = slices_clone(a, expanded);
    ((Byte *)bad.p)[0] = 0xff;
    ((Byte *)bad.p)[1] |= 0x0f;
    burrow__mlkem_testing_only_new_decapsulation_key768(a, bad, &err);
    want_error(t, "TestingOnlyNewDecapsulationKey768", err,
               "mlkem: invalid secret key encoding");
    bad = slices_clone(a, expanded);
    ((Byte *)bad.p)[3 * MLKEM_ENCODING_SIZE12] = 0xff;
    ((Byte *)bad.p)[3 * MLKEM_ENCODING_SIZE12 + 1] |= 0x0f;
    burrow__mlkem_testing_only_new_decapsulation_key768(a, bad, &err);
    want_error(t, "TestingOnlyNewDecapsulationKey768", err,
               "mlkem: invalid polynomial encoding");
    bad = slices_clone(a, expanded);
    ((Byte *)bad.p)[MLKEM_DECAPSULATION_KEY_SIZE768 - 64] ^= 1;
    burrow__mlkem_testing_only_new_decapsulation_key768(a, bad, &err);
    want_error(t, "TestingOnlyNewDecapsulationKey768", err,
               "mlkem: inconsistent H(ek) in encoded bytes");
    arena_free(&ar);
}

/* Keys from the heap, freed one by one, and the encapsulation key of a
 * decapsulation key, which its free leaves alone. */
static void TestHeapKeys(TestingT *t) {
    Error err = BURROW_NO_ERROR;
    MlkemDecapsulationKey768 *dk = mlkem_generate_key768(NULL, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    MlkemEncapsulationKey768 *own = (MlkemEncapsulationKey768 *)(uintptr_t)
        mlkem_decapsulation_key768_encapsulation_key(dk);
    mlkem_encapsulation_key768_free(own);
    Slice ek_bytes = mlkem_encapsulation_key768_bytes(own, NULL);
    MlkemEncapsulationKey768 *ek = mlkem_new_encapsulation_key768(NULL, ek_bytes, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    Slice c;
    Slice ke = mlkem_encapsulation_key768_encapsulate(ek, NULL, &c);
    Slice kd = mlkem_decapsulation_key768_decapsulate(dk, NULL, c, &err);
    if (BURROW_FAILED(err) || !bytes_equal(ke, kd))
        testing_t_errorf_v(t, "round trip with heap keys failed");
    Slice owned[] = {ek_bytes, c, ke, kd};
    for (size_t i = 0; i < sizeof owned / sizeof owned[0]; i++)
        mem_free(heap_allocator(), owned[i].p, (size_t)owned[i].cap, 1);
    mlkem_encapsulation_key768_free(ek);
    mlkem_decapsulation_key768_free(dk);
    mlkem_encapsulation_key1024_free(NULL);
    mlkem_decapsulation_key1024_free(NULL);

    MlkemDecapsulationKey1024 *dk1024 = mlkem_generate_key1024(NULL, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    mlkem_decapsulation_key1024_free(dk1024);
}

/* ------------------------------------------------------------ benchmarks */

static volatile Byte sink;

static void BenchmarkKeyGen(TestingB *b) {
    Byte d[32], z[32];
    read_random(bs(d, 32));
    read_random(bs(z, 32));
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    testing_b_reset_timer(b);
    for (Int i = 0; i < testing_b_n(b); i++) {
        MlkemDecapsulationKey768 *dk = burrow__mlkem_generate_key_internal768(a, d, z);
        Slice ek = mlkem_encapsulation_key768_bytes(
            mlkem_decapsulation_key768_encapsulation_key(dk), a);
        sink ^= ((Byte *)ek.p)[0];
        arena_reset(&ar);
    }
    arena_free(&ar);
}

static void BenchmarkEncaps(TestingB *b) {
    Byte seed[MLKEM_SEED_SIZE];
    read_random(bs(seed, sizeof seed));
    Arena keys, ar;
    arena_init(&keys, NULL, 0);
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    MlkemDecapsulationKey768 *dk = mlkem_new_decapsulation_key768(
        arena_allocator(&keys), bs(seed, sizeof seed), &err);
    if (BURROW_FAILED(err))
        testing_b_fatalf_v(b, "%s", error_text(err));
    Slice ek_bytes = mlkem_encapsulation_key768_bytes(
        mlkem_decapsulation_key768_encapsulation_key(dk), arena_allocator(&keys));
    testing_b_reset_timer(b);
    for (Int i = 0; i < testing_b_n(b); i++) {
        MlkemEncapsulationKey768 *ek =
            mlkem_new_encapsulation_key768(a, ek_bytes, &err);
        if (BURROW_FAILED(err))
            testing_b_fatalf_v(b, "%s", error_text(err));
        Slice c;
        Slice k = mlkem_encapsulation_key768_encapsulate(ek, a, &c);
        sink ^= ((Byte *)c.p)[0] ^ ((Byte *)k.p)[0];
        arena_reset(&ar);
    }
    arena_free(&ar);
    arena_free(&keys);
}

static void BenchmarkDecaps(TestingB *b) {
    Arena keys, ar;
    arena_init(&keys, NULL, 0);
    arena_init(&ar, NULL, 0);
    Alloc *ka = arena_allocator(&keys), *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    MlkemDecapsulationKey768 *dk = mlkem_generate_key768(ka, &err);
    if (BURROW_FAILED(err))
        testing_b_fatalf_v(b, "%s", error_text(err));
    Slice c;
    mlkem_encapsulation_key768_encapsulate(
        mlkem_decapsulation_key768_encapsulation_key(dk), ka, &c);
    testing_b_reset_timer(b);
    for (Int i = 0; i < testing_b_n(b); i++) {
        Slice k = mlkem_decapsulation_key768_decapsulate(dk, a, c, &err);
        sink ^= ((Byte *)k.p)[0];
        arena_reset(&ar);
    }
    arena_free(&ar);
    arena_free(&keys);
}

typedef struct RoundTripBench {
    MlkemDecapsulationKey768 *dk;
    Slice ek_bytes, c;
} RoundTripBench;

static void round_trip_alice(void *env, TestingB *b) {
    const RoundTripBench *s = env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < testing_b_n(b); i++) {
        Error err = BURROW_NO_ERROR;
        MlkemDecapsulationKey768 *dk_s = mlkem_generate_key768(a, &err);
        if (BURROW_FAILED(err))
            testing_b_fatalf_v(b, "%s", error_text(err));
        Slice ek_s = mlkem_encapsulation_key768_bytes(
            mlkem_decapsulation_key768_encapsulation_key(dk_s), a);
        sink ^= ((Byte *)ek_s.p)[0];

        Slice ks = mlkem_decapsulation_key768_decapsulate(s->dk, a, s->c, &err);
        if (BURROW_FAILED(err))
            testing_b_fatalf_v(b, "%s", error_text(err));
        sink ^= ((Byte *)ks.p)[0];
        arena_reset(&ar);
    }
    arena_free(&ar);
}

static void round_trip_bob(void *env, TestingB *b) {
    const RoundTripBench *s = env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < testing_b_n(b); i++) {
        Error err = BURROW_NO_ERROR;
        MlkemEncapsulationKey768 *ek =
            mlkem_new_encapsulation_key768(a, s->ek_bytes, &err);
        if (BURROW_FAILED(err))
            testing_b_fatalf_v(b, "%s", error_text(err));
        Slice cs;
        Slice ks = mlkem_encapsulation_key768_encapsulate(ek, a, &cs);
        sink ^= ((Byte *)cs.p)[0] ^ ((Byte *)ks.p)[0];
        arena_reset(&ar);
    }
    arena_free(&ar);
}

static void BenchmarkRoundTrip(TestingB *b) {
    Arena keys;
    arena_init(&keys, NULL, 0);
    Alloc *ka = arena_allocator(&keys);
    Error err = BURROW_NO_ERROR;
    RoundTripBench s;
    s.dk = mlkem_generate_key768(ka, &err);
    if (BURROW_FAILED(err))
        testing_b_fatalf_v(b, "%s", error_text(err));
    const MlkemEncapsulationKey768 *ek =
        mlkem_decapsulation_key768_encapsulation_key(s.dk);
    s.ek_bytes = mlkem_encapsulation_key768_bytes(ek, ka);
    mlkem_encapsulation_key768_encapsulate(ek, ka, &s.c);
    testing_b_run(b, BURROW_S("Alice"), BURROW_FN(TestingBFunc, round_trip_alice, &s));
    testing_b_run(b, BURROW_S("Bob"), BURROW_FN(TestingBFunc, round_trip_bob, &s));
    arena_free(&keys);
}

#define TESTS(X)                                                                       \
    X(TestFieldReduce)                                                                 \
    X(TestFieldAdd)                                                                    \
    X(TestFieldSub)                                                                    \
    X(TestFieldMul)                                                                    \
    X(TestDecompressCompress)                                                          \
    X(TestCompress)                                                                    \
    X(TestDecompress)                                                                  \
    X(TestEncodeDecode)                                                                \
    X(TestZetas)                                                                       \
    X(TestGammas)                                                                      \
    X(TestRoundTrip)                                                                   \
    X(TestBadLengths)                                                                  \
    X(TestAccumulated)                                                                 \
    X(TestConstantSizes)                                                               \
    X(TestKeyGenWycheproof)                                                            \
    X(TestMLKEMEncapsWycheproof)                                                       \
    X(TestMLKEMDecapsWycheproof)                                                       \
    X(TestMLKEMSemiExpandedDecapsWycheproof)                                           \
    X(TestCAST)                                                                        \
    X(TestInterfaces)                                                                  \
    X(TestErrors)                                                                      \
    X(TestHeapKeys)                                                                    \
    X(BenchmarkKeyGen)                                                                 \
    X(BenchmarkEncaps)                                                                 \
    X(BenchmarkDecaps)                                                                 \
    X(BenchmarkRoundTrip)

TESTING_MAIN(TESTS)
