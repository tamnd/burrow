/* Derived from Go's src/hash/maphash/smhasher_test.go.
 * Go source: go1.27.1.
 *
 * Smhasher is a torture test for hash functions. These are the tests that
 * check how the hash collides and how its bits mix, and since the runtime's
 * hash is burrow's own and not Go's, these are the tests that say it is good
 * enough. Go seeds math/rand with 1234 for the random inputs. That package is
 * not ported yet, so a splitmix64 generator with the same seed stands in.
 *
 * Copyright 2019 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/hash/maphash.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"

#include <string.h>

static MaphashSeed fixed_seed;

typedef struct Rand {
    uint64_t s;
} Rand;

static void rand_bytes(Rand *r, Byte *b, Int n) {
    for (Int i = 0; i < n; i++) {
        uint64_t z = (r->s += 0x9e3779b97f4a7c15ULL);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        b[i] = (Byte)(z ^ (z >> 31));
    }
}

static uint64_t bytes_hash(const Byte *b, Int n) {
    MaphashHash h = {0};
    maphash_hash_set_seed(&h, fixed_seed);
    maphash_hash_write(&h, slice_from((void *)(uintptr_t)b, n, n, TYPE_BYTE), NULL);
    return maphash_hash_sum64(&h);
}

static uint64_t string_hash(Str s) {
    MaphashHash h = {0};
    maphash_hash_set_seed(&h, fixed_seed);
    maphash_hash_write_string(&h, s, NULL);
    return maphash_hash_sum64(&h);
}

static void TestSmhasherSanity(TestingT *t) {
    Rand r = {1234};
    enum { REP = 10, KEYMAX = 128, PAD = 16, OFFMAX = 16 };
    for (int k = 0; k < REP; k++) {
        for (int n = 0; n < KEYMAX; n++) {
            for (int i = 0; i < OFFMAX; i++) {
                Byte b[KEYMAX + OFFMAX + 2 * PAD];
                Byte c[KEYMAX + OFFMAX + 2 * PAD];
                rand_bytes(&r, b, (Int)sizeof b);
                rand_bytes(&r, c, (Int)sizeof c);
                memcpy(c + PAD + i, b + PAD, (size_t)n);
                if (bytes_hash(b + PAD, n) != bytes_hash(c + PAD + i, n))
                    testing_t_errorf_v(t, "hash depends on bytes outside key");
            }
        }
    }
}

enum { HASH_SIZE = 64 };

/* The hashes added so far, in an arena of their own since some of the tests
 * add millions and each growth leaves the old array behind. */
typedef struct HashSet {
    Arena ar;
    Slice list;
} HashSet;

static void hs_init(HashSet *s) {
    arena_init(&s->ar, heap_allocator(), 0);
    s->list = slice_make(arena_allocator(&s->ar), TYPE_UINT64, 0, 1024);
}

static void hs_free(HashSet *s) {
    arena_free(&s->ar);
}

static void hs_add(HashSet *s, uint64_t h) {
    s->list = slice_append(arena_allocator(&s->ar), s->list, &h, 1);
}

static void hs_add_b(HashSet *s, const Byte *b, Int n) {
    hs_add(s, bytes_hash(b, n));
}

static void hs_add_s(HashSet *s, Str x) {
    hs_add(s, string_hash(x));
}

static void hs_add_s_seed(HashSet *s, Str x, MaphashSeed seed) {
    MaphashHash h = {0};
    maphash_hash_set_seed(&h, seed);
    maphash_hash_write_string(&h, x, NULL);
    hs_add(s, maphash_hash_sum64(&h));
}

static void hs_check(TestingT *t, HashSet *s) {
    Slice list = s->list;
    slices_sort(list);
    int64_t collisions = 0;
    for (Int i = 1; i < list.len; i++) {
        if (BURROW_AT(uint64_t, list, i) == BURROW_AT(uint64_t, list, i - 1))
            collisions++;
    }
    int64_t n = list.len;
    const double slop = 10.0;
    int64_t pairs = n * (n - 1) / 2;
    double expected = (double)pairs / math_pow(2.0, HASH_SIZE);
    double stddev = math_sqrt(expected);
    if ((double)collisions > expected + slop * (3 * stddev + 1))
        testing_t_errorf_v(t,
                           "unexpected number of collisions: got=%d mean=%f stddev=%f",
                           collisions, expected, stddev);
    s->list.len = 0;
}

static void TestSmhasherAppendedZeros(TestingT *t) {
    Byte s[5 + 256] = "hello";
    HashSet h;
    hs_init(&h);
    for (Int i = 0; i <= (Int)sizeof s; i++)
        hs_add_s(&h, (Str){s, i});
    hs_check(t, &h);
    hs_free(&h);
}

static void TestSmhasherSmallKeys(TestingT *t) {
    HashSet h;
    hs_init(&h);
    Byte b[3];
    for (int i = 0; i < 256; i++) {
        b[0] = (Byte)i;
        hs_add_b(&h, b, 1);
        for (int j = 0; j < 256; j++) {
            b[1] = (Byte)j;
            hs_add_b(&h, b, 2);
            if (!testing_short()) {
                for (int k = 0; k < 256; k++) {
                    b[2] = (Byte)k;
                    hs_add_b(&h, b, 3);
                }
            }
        }
    }
    hs_check(t, &h);
    hs_free(&h);
}

static void TestSmhasherZeros(TestingT *t) {
    Int n = testing_short() ? 1024 : 256 * 1024;
    HashSet h;
    hs_init(&h);
    Slice b = slice_make(heap_allocator(), TYPE_BYTE, n, n);
    for (Int i = 0; i <= n; i++)
        hs_add_b(&h, b.p, i);
    hs_check(t, &h);
    mem_free(heap_allocator(), b.p, (size_t)n, 1);
    hs_free(&h);
}

static void two_non_zero(HashSet *h, int n) {
    Byte b[16] = {0};
    hs_add_b(h, b, n);
    for (int i = 0; i < n; i++) {
        for (int x = 1; x < 256; x++) {
            b[i] = (Byte)x;
            hs_add_b(h, b, n);
            b[i] = 0;
        }
    }
    for (int i = 0; i < n; i++) {
        for (int x = 1; x < 256; x++) {
            b[i] = (Byte)x;
            for (int j = i + 1; j < n; j++) {
                for (int y = 1; y < 256; y++) {
                    b[j] = (Byte)y;
                    hs_add_b(h, b, n);
                    b[j] = 0;
                }
            }
            b[i] = 0;
        }
    }
}

static void TestSmhasherTwoNonzero(TestingT *t) {
    if (testing_short()) {
        testing_t_skipf_v(t, "Skipping in short mode");
        return;
    }
    HashSet h;
    hs_init(&h);
    for (int n = 2; n <= 16; n++)
        two_non_zero(&h, n);
    hs_check(t, &h);
    hs_free(&h);
}

static void TestSmhasherCyclic(TestingT *t) {
    if (testing_short()) {
        testing_t_skipf_v(t, "Skipping in short mode");
        return;
    }
    Rand r = {1234};
    enum { REPEAT = 8, N = 1000000 };
    HashSet h;
    hs_init(&h);
    for (int n = 4; n <= 12; n++) {
        Byte b[REPEAT * 12];
        for (int i = 0; i < N; i++) {
            b[0] = (Byte)(i * 79 % 97);
            b[1] = (Byte)(i * 43 % 137);
            b[2] = (Byte)(i * 151 % 197);
            b[3] = (Byte)(i * 199 % 251);
            rand_bytes(&r, b + 4, n - 4);
            for (int j = n; j < n * REPEAT; j++)
                b[j] = b[j - n];
            hs_add_b(&h, b, (Int)n * REPEAT);
        }
        hs_check(t, &h);
    }
    hs_free(&h);
}

static void setbits(HashSet *h, Byte *b, Int nb, Int i, int k) {
    hs_add_b(h, b, nb);
    if (k == 0)
        return;
    for (Int j = i; j < nb * 8; j++) {
        b[j / 8] |= (Byte)(1U << (j & 7));
        setbits(h, b, nb, j + 1, k - 1);
        b[j / 8] &= (Byte) ~(1U << (j & 7));
    }
}

static void sparse(TestingT *t, HashSet *h, int n, int k) {
    Byte b[2048 / 8] = {0};
    setbits(h, b, n / 8, 0, k);
    hs_check(t, h);
}

static void TestSmhasherSparse(TestingT *t) {
    if (testing_short()) {
        testing_t_skipf_v(t, "Skipping in short mode");
        return;
    }
    HashSet h;
    hs_init(&h);
    sparse(t, &h, 32, 6);
    sparse(t, &h, 40, 6);
    sparse(t, &h, 48, 5);
    sparse(t, &h, 56, 5);
    sparse(t, &h, 64, 5);
    sparse(t, &h, 96, 4);
    sparse(t, &h, 256, 3);
    sparse(t, &h, 2048, 2);
    hs_free(&h);
}

static void gen_perm(HashSet *h, Byte *b, Int len, const uint32_t *s, Int ns, Int n) {
    hs_add_b(h, b, n);
    if (n == len)
        return;
    for (Int i = 0; i < ns; i++) {
        uint32_t v = s[i];
        b[n] = (Byte)v;
        b[n + 1] = (Byte)(v >> 8);
        b[n + 2] = (Byte)(v >> 16);
        b[n + 3] = (Byte)(v >> 24);
        gen_perm(h, b, len, s, ns, n + 4);
    }
}

static void permutation(TestingT *t, HashSet *h, const uint32_t *s, Int ns, Int n) {
    Byte b[20 * 4];
    gen_perm(h, b, n * 4, s, ns, 0);
    hs_check(t, h);
}

#define PERM(h, n, ...)                                                                \
    permutation(t, (h), (const uint32_t[]){__VA_ARGS__},                               \
                (Int)(sizeof((const uint32_t[]){__VA_ARGS__}) / sizeof(uint32_t)),     \
                (n))

static void TestSmhasherPermutation(TestingT *t) {
    if (testing_short()) {
        testing_t_skipf_v(t, "Skipping in short mode");
        return;
    }
    HashSet h;
    hs_init(&h);
    PERM(&h, 8, 0, 1, 2, 3, 4, 5, 6, 7);
    PERM(&h, 8, 0, 1U << 29, 2U << 29, 3U << 29, 4U << 29, 5U << 29, 6U << 29,
         7U << 29);
    PERM(&h, 20, 0, 1);
    PERM(&h, 20, 0, 1U << 31);
    PERM(&h, 6, 0, 1, 2, 3, 4, 5, 6, 7, 1U << 29, 2U << 29, 3U << 29, 4U << 29,
         5U << 29, 6U << 29, 7U << 29);
    hs_free(&h);
}

static void flip_bit(Byte *b, int i) {
    b[i >> 3] ^= (Byte)(1U << (i & 7));
}

/* Go's avalancheTest1 over a bytesKey of n bytes. */
static void avalanche_test1(TestingT *t, int nbytes) {
    enum { REP = 100000 };
    Rand r = {1234};
    Byte k[200];
    int n = nbytes * 8;
    Slice grid =
        slice_make(heap_allocator(), TYPE_INT, (Int)n * HASH_SIZE, (Int)n * HASH_SIZE);
    Int *g = grid.p;

    for (int z = 0; z < REP; z++) {
        rand_bytes(&r, k, nbytes);
        uint64_t h = bytes_hash(k, nbytes);
        for (int i = 0; i < n; i++) {
            flip_bit(k, i);
            uint64_t d = h ^ bytes_hash(k, nbytes);
            flip_bit(k, i);
            for (int j = 0; j < HASH_SIZE; j++) {
                g[i * HASH_SIZE + j] += (Int)(d & 1);
                d >>= 1;
            }
        }
    }

    /* The bound Go works out: how many standard deviations make every one of
     * the n*64 cells land inside with probability .9999, times 11 for slack. */
    double big_n = (double)n * (double)HASH_SIZE;
    int tenths = 0;
    while (math_pow(math_erf(tenths * .1 / math_sqrt(2)), big_n) < .9999)
        tenths++;
    double c = tenths * .1 * 11.0;
    double mean = .5 * (double)REP;
    double stddev = .5 * math_sqrt((double)REP);
    Int low = (Int)(mean - c * stddev);
    Int high = (Int)(mean + c * stddev);
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < HASH_SIZE; j++) {
            Int x = g[i * HASH_SIZE + j];
            if (x < low || x > high)
                testing_t_errorf_v(t, "bad bias for bytes%d bit %d -> bit %d: %d/%d",
                                   nbytes, i, j, x, REP);
        }
    }
    mem_free(heap_allocator(), grid.p, (size_t)grid.cap * sizeof(Int), _Alignof(Int));
}

static void TestSmhasherAvalanche(TestingT *t) {
    if (testing_short()) {
        testing_t_skipf_v(t, "Skipping in short mode");
        return;
    }
    avalanche_test1(t, 2);
    avalanche_test1(t, 4);
    avalanche_test1(t, 8);
    avalanche_test1(t, 16);
    avalanche_test1(t, 32);
    avalanche_test1(t, 200);
}

static void TestSmhasherWindowed(TestingT *t) {
    if (testing_short()) {
        testing_t_skipf_v(t, "Skipping in short mode");
        return;
    }
    enum { BITS = 16, NBYTES = 128 };
    Byte k[NBYTES];
    int nbits = NBYTES * 8;
    HashSet h;
    hs_init(&h);
    for (int r = 0; r < nbits; r++) {
        for (int i = 0; i < 1 << BITS; i++) {
            memset(k, 0, sizeof k);
            for (int j = 0; j < BITS; j++) {
                if ((i >> j) & 1)
                    flip_bit(k, (j + r) % nbits);
            }
            hs_add(&h, bytes_hash(k, NBYTES));
        }
        hs_check(t, &h);
    }
    hs_free(&h);
}

static void text(TestingT *t, HashSet *h, const char *prefix, const char *suffix) {
    enum { N = 4 };
    static const char S[] = "ABCDEFGHIJKLMNOPQRSTabcdefghijklmnopqrst0123456789";
    const int L = (int)sizeof S - 1;
    Byte b[16];
    Int np = (Int)strlen(prefix), ns = (Int)strlen(suffix);
    Int n = np + N + ns;
    memcpy(b, prefix, (size_t)np);
    memcpy(b + np + N, suffix, (size_t)ns);
    Byte *c = b + np;
    for (int i = 0; i < L; i++) {
        c[0] = (Byte)S[i];
        for (int j = 0; j < L; j++) {
            c[1] = (Byte)S[j];
            for (int k = 0; k < L; k++) {
                c[2] = (Byte)S[k];
                for (int x = 0; x < L; x++) {
                    c[3] = (Byte)S[x];
                    hs_add_b(h, b, n);
                }
            }
        }
    }
    hs_check(t, h);
}

static void TestSmhasherText(TestingT *t) {
    if (testing_short()) {
        testing_t_skipf_v(t, "Skipping in short mode");
        return;
    }
    HashSet h;
    hs_init(&h);
    text(t, &h, "Foo", "Bar");
    text(t, &h, "FooBar", "");
    text(t, &h, "", "FooBar");
    hs_free(&h);
}

static void TestSmhasherSeed(TestingT *t) {
    HashSet h;
    hs_init(&h);
    enum { N = 100000 };
    Str s = BURROW_S("hello");
    for (uint64_t i = 0; i < N; i++) {
        hs_add_s_seed(&h, s, (MaphashSeed){i + 1});
        hs_add_s_seed(&h, s, (MaphashSeed){(i + 1) << 32});
    }
    hs_check(t, &h);
    hs_free(&h);
}

static int TestMain(TestingM *m) {
    fixed_seed = maphash_make_seed();
    return testing_m_run(m);
}

#define TESTS(X)                                                                       \
    X(TestSmhasherSanity)                                                              \
    X(TestSmhasherAppendedZeros)                                                       \
    X(TestSmhasherSmallKeys)                                                           \
    X(TestSmhasherZeros)                                                               \
    X(TestSmhasherTwoNonzero)                                                          \
    X(TestSmhasherCyclic)                                                              \
    X(TestSmhasherSparse)                                                              \
    X(TestSmhasherPermutation)                                                         \
    X(TestSmhasherAvalanche)                                                           \
    X(TestSmhasherWindowed)                                                            \
    X(TestSmhasherText)                                                                \
    X(TestSmhasherSeed)

TESTING_MAIN_WITH(TestMain, TESTS)
