/* Derived from Go's src/math/rand/regress_test.go, rand_test.go and
 * auto_test.go, and from src/math/rand/v2/regress_test.go, pcg_test.go,
 * chacha8_test.go and rand_test.go.
 * Go source: go1.27.1.
 *
 * The regression tables are in rand_test_gen.h, read out of Go's tests by
 * tools/gen-rand-tests.sh. Go's TestRegress walks the methods of Rand with
 * reflect, which gives them in name order, and the tables below list the same
 * methods in the same order. The distribution tests, TestUniformFactorial and
 * the race tests are not ported: the regression tables pin every value those
 * would look at, and the shared generators are exercised from many threads by
 * TestConcurrent here instead.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/crypto/sha256.h"
#include "burrow/encoding/hex.h"
#include "burrow/math/rand.h"
#include "burrow/math/rand/v2.h"
#include "burrow/mem/arena.h"
#include "burrow/sync.h"

#include <string.h>

enum { RG_F64, RG_F32, RG_INT32, RG_INT64, RG_UINT32, RG_UINT64, RG_INTS, RG_BYTES };

typedef struct RandGolden {
    const char *name;
    int kind;
    uint64_t bits;
    const int64_t *items;
    Int n;
} RandGolden;

typedef struct RandEnc {
    const Byte *p;
    Int n;
} RandEnc;

#include "rand_test_gen.h"

/* What a call gave, in the shape of a golden row. */
typedef struct Got {
    int kind;
    uint64_t bits;
    int64_t items[16];
    Int n;
} Got;

static uint64_t f64_bits(double f) {
    uint64_t u;
    memcpy(&u, &f, sizeof u);
    return u;
}

static uint64_t f32_bits(float f) {
    uint32_t u;
    memcpy(&u, &f, sizeof u);
    return u;
}

static void check_golden(TestingT *t, const RandGolden *want, const char *name,
                         const char *arg, const Got *got) {
    if (strcmp(want->name, name) != 0) {
        testing_t_fatalf_v(t, "golden row is for %s, the test is at %s", want->name,
                           name);
    }
    bool ok = want->kind == got->kind;
    if (ok && (got->kind == RG_INTS || got->kind == RG_BYTES)) {
        ok = want->n == got->n;
        for (Int i = 0; ok && i < got->n; i++)
            ok = want->items[i] == got->items[i];
    } else if (ok) {
        ok = want->bits == got->bits;
    }
    if (!ok)
        testing_t_errorf_v(t, "r.%s(%s) = %#x (%d items), want %#x (%d items)", name,
                           arg, got->bits, got->n, want->bits, want->n);
}

static const int32_t int32s[] = {
    1, 10, 32, 1 << 20, (1 << 20) + 1, 1000000000, 1 << 30, INT32_MAX - 1, INT32_MAX};
static const uint32_t uint32s[] = {1,
                                   10,
                                   32,
                                   1 << 20,
                                   (1 << 20) + 1,
                                   1000000000,
                                   1 << 30,
                                   INT32_MAX - 1,
                                   INT32_MAX,
                                   UINT32_MAX - 1,
                                   UINT32_MAX};
static const int64_t int64s[] = {1,
                                 10,
                                 32,
                                 1 << 20,
                                 (1 << 20) + 1,
                                 1000000000,
                                 1 << 30,
                                 INT32_MAX - 1,
                                 INT32_MAX,
                                 INT64_C(1000000000000000000),
                                 INT64_C(1) << 60,
                                 INT64_MAX - 1,
                                 INT64_MAX};
static const uint64_t uint64s[] = {1,
                                   10,
                                   32,
                                   1 << 20,
                                   (1 << 20) + 1,
                                   1000000000,
                                   1 << 30,
                                   INT32_MAX - 1,
                                   INT32_MAX,
                                   UINT64_C(1000000000000000000),
                                   UINT64_C(1) << 60,
                                   (uint64_t)INT64_MAX - 1,
                                   (uint64_t)INT64_MAX,
                                   UINT64_MAX - 1,
                                   UINT64_MAX};
static const Int perm_sizes[] = {0, 1, 5, 8, 9, 10, 16};
static const Int read_sizes[] = {1, 7, 8, 9, 10};

#define PICK(arr, i) (arr[(i) % (Int)(sizeof(arr) / sizeof(arr[0]))])

/* ---------------------------------------------------------------- v1 */

static const char *const v1_methods[] = {
    "ExpFloat64", "Float32", "Float64",     "Int",  "Int31", "Int31n", "Int63",
    "Int63n",     "Intn",    "NormFloat64", "Perm", "Read",  "Uint32", "Uint64",
};

static void v1_call(MathRandRand *r, Alloc *a, const char *m, Int repeat, Got *g,
                    char *arg, size_t argn) {
    memset(g, 0, sizeof *g);
    arg[0] = '\0';
    if (strcmp(m, "ExpFloat64") == 0) {
        g->kind = RG_F64;
        g->bits = f64_bits(math_rand_rand_exp_float64(r));
    } else if (strcmp(m, "Float32") == 0) {
        g->kind = RG_F32;
        g->bits = f32_bits(math_rand_rand_float32(r));
    } else if (strcmp(m, "Float64") == 0) {
        g->kind = RG_F64;
        g->bits = f64_bits(math_rand_rand_float64(r));
    } else if (strcmp(m, "Int") == 0) {
        g->kind = RG_INT64;
        g->bits = (uint64_t)(int64_t)math_rand_rand_int(r);
    } else if (strcmp(m, "Int31") == 0) {
        g->kind = RG_INT32;
        g->bits = (uint64_t)(int64_t)math_rand_rand_int31(r);
    } else if (strcmp(m, "Int31n") == 0) {
        int32_t x = PICK(int32s, repeat);
        snprintf(arg, argn, "%d", (int)x);
        g->kind = RG_INT32;
        g->bits = (uint64_t)(int64_t)math_rand_rand_int31n(r, x);
    } else if (strcmp(m, "Int63") == 0) {
        g->kind = RG_INT64;
        g->bits = (uint64_t)math_rand_rand_int63(r);
    } else if (strcmp(m, "Int63n") == 0) {
        int64_t x = PICK(int64s, repeat);
        snprintf(arg, argn, "%lld", (long long)x);
        g->kind = RG_INT64;
        g->bits = (uint64_t)math_rand_rand_int63n(r, x);
    } else if (strcmp(m, "Intn") == 0) {
        int64_t x = PICK(int64s, repeat);
        snprintf(arg, argn, "%lld", (long long)x);
        g->kind = RG_INT64;
        if ((int64_t)(Int)x != x) {
            /* What a 64 bit machine would draw, to keep the stream in step.
             * The golden row is then not comparable. */
            (void)math_rand_rand_int63n(r, x);
            g->kind = -1;
        } else {
            g->bits = (uint64_t)(int64_t)math_rand_rand_intn(r, (Int)x);
        }
    } else if (strcmp(m, "NormFloat64") == 0) {
        g->kind = RG_F64;
        g->bits = f64_bits(math_rand_rand_norm_float64(r));
    } else if (strcmp(m, "Perm") == 0) {
        Int n = PICK(perm_sizes, repeat);
        snprintf(arg, argn, "%d", (int)n);
        Slice p = math_rand_rand_perm(r, a, n);
        g->kind = RG_INTS;
        g->n = p.len;
        for (Int i = 0; i < p.len; i++)
            g->items[i] = ((const Int *)p.p)[i];
    } else if (strcmp(m, "Read") == 0) {
        Int n = PICK(read_sizes, repeat);
        snprintf(arg, argn, "[%d bytes]", (int)n);
        Byte buf[16] = {0};
        Error err;
        Int k = math_rand_rand_read(r, slice_from(buf, n, n, TYPE_BYTE), &err);
        g->kind = BURROW_OK(err) && k == n ? RG_BYTES : -1;
        g->n = n;
        for (Int i = 0; i < n; i++)
            g->items[i] = buf[i];
    } else if (strcmp(m, "Uint32") == 0) {
        g->kind = RG_UINT32;
        g->bits = math_rand_rand_uint32(r);
    } else if (strcmp(m, "Uint64") == 0) {
        g->kind = RG_UINT64;
        g->bits = math_rand_rand_uint64(r);
    }
}

static void TestRegress(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    MathRandRand *r = math_rand_new(a, math_rand_new_source(a, 0));
    Int p = 0;
    Int rows = (Int)(sizeof rand_v1_golden / sizeof rand_v1_golden[0]);
    for (size_t i = 0; i < sizeof v1_methods / sizeof v1_methods[0]; i++) {
        math_rand_rand_seed(r, 0);
        for (Int repeat = 0; repeat < 20; repeat++, p++) {
            Got g;
            char arg[64];
            v1_call(r, a, v1_methods[i], repeat, &g, arg, sizeof arg);
            if (p >= rows)
                testing_t_fatalf_v(t, "r.%s(%s): missing golden value", v1_methods[i],
                                   arg);
            if (g.kind != -1)
                check_golden(t, &rand_v1_golden[p], v1_methods[i], arg, &g);
        }
    }
    CHECK_INT_EQ(p, rows);
    arena_free(&ar);
}

/* ---------------------------------------------------------------- v2 */

static const char *const v2_methods[] = {
    "ExpFloat64", "Float32", "Float64", "Int",         "Int32", "Int32N",
    "Int64",      "Int64N",  "IntN",    "NormFloat64", "Perm",  "Uint",
    "Uint32",     "Uint32N", "Uint64",  "Uint64N",     "UintN",
};

static void v2_call(Mathrand2Rand *r, Alloc *a, const char *m, Int repeat, Got *g,
                    char *arg, size_t argn) {
    memset(g, 0, sizeof *g);
    arg[0] = '\0';
    if (strcmp(m, "ExpFloat64") == 0) {
        g->kind = RG_F64;
        g->bits = f64_bits(mathrand2_rand_exp_float64(r));
    } else if (strcmp(m, "Float32") == 0) {
        g->kind = RG_F32;
        g->bits = f32_bits(mathrand2_rand_float32(r));
    } else if (strcmp(m, "Float64") == 0) {
        g->kind = RG_F64;
        g->bits = f64_bits(mathrand2_rand_float64(r));
    } else if (strcmp(m, "Int") == 0) {
        g->kind = RG_INT64;
        g->bits = (uint64_t)(int64_t)mathrand2_rand_int(r);
    } else if (strcmp(m, "Int32") == 0) {
        g->kind = RG_INT32;
        g->bits = (uint64_t)(int64_t)mathrand2_rand_int32(r);
    } else if (strcmp(m, "Int32N") == 0) {
        int32_t x = PICK(int32s, repeat);
        snprintf(arg, argn, "%d", (int)x);
        g->kind = RG_INT32;
        g->bits = (uint64_t)(int64_t)mathrand2_rand_int32_n(r, x);
    } else if (strcmp(m, "Int64") == 0) {
        g->kind = RG_INT64;
        g->bits = (uint64_t)mathrand2_rand_int64(r);
    } else if (strcmp(m, "Int64N") == 0) {
        int64_t x = PICK(int64s, repeat);
        snprintf(arg, argn, "%lld", (long long)x);
        g->kind = RG_INT64;
        g->bits = (uint64_t)mathrand2_rand_int64_n(r, x);
    } else if (strcmp(m, "IntN") == 0) {
        int64_t x = PICK(int64s, repeat);
        snprintf(arg, argn, "%lld", (long long)x);
        g->kind = RG_INT64;
        if ((int64_t)(Int)x != x) {
            (void)mathrand2_rand_int64_n(r, x);
            g->kind = -1;
        } else {
            g->bits = (uint64_t)(int64_t)mathrand2_rand_int_n(r, (Int)x);
        }
    } else if (strcmp(m, "NormFloat64") == 0) {
        g->kind = RG_F64;
        g->bits = f64_bits(mathrand2_rand_norm_float64(r));
    } else if (strcmp(m, "Perm") == 0) {
        Int n = PICK(perm_sizes, repeat);
        snprintf(arg, argn, "%d", (int)n);
        Slice p = mathrand2_rand_perm(r, a, n);
        g->kind = RG_INTS;
        g->n = p.len;
        for (Int i = 0; i < p.len; i++)
            g->items[i] = ((const Int *)p.p)[i];
    } else if (strcmp(m, "Uint") == 0) {
        g->kind = RG_UINT64;
        g->bits = (uint64_t)mathrand2_rand_uint(r);
        if (sizeof(Uint) < 8)
            g->kind = -1;
    } else if (strcmp(m, "Uint32") == 0) {
        g->kind = RG_UINT32;
        g->bits = mathrand2_rand_uint32(r);
    } else if (strcmp(m, "Uint32N") == 0) {
        uint32_t x = PICK(uint32s, repeat);
        snprintf(arg, argn, "%u", (unsigned)x);
        g->kind = RG_UINT32;
        g->bits = mathrand2_rand_uint32_n(r, x);
    } else if (strcmp(m, "Uint64") == 0) {
        g->kind = RG_UINT64;
        g->bits = mathrand2_rand_uint64(r);
    } else if (strcmp(m, "Uint64N") == 0) {
        uint64_t x = PICK(uint64s, repeat);
        snprintf(arg, argn, "%llu", (unsigned long long)x);
        g->kind = RG_UINT64;
        g->bits = mathrand2_rand_uint64_n(r, x);
    } else if (strcmp(m, "UintN") == 0) {
        uint64_t x = PICK(uint64s, repeat);
        snprintf(arg, argn, "%llu", (unsigned long long)x);
        g->kind = RG_UINT64;
        if ((uint64_t)(Uint)x != x) {
            (void)mathrand2_rand_uint64_n(r, x);
            g->kind = -1;
        } else {
            g->bits = (uint64_t)mathrand2_rand_uint_n(r, (Uint)x);
        }
    }
}

static void TestRegressV2(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Int p = 0;
    Int rows = (Int)(sizeof rand_v2_golden / sizeof rand_v2_golden[0]);
    for (size_t i = 0; i < sizeof v2_methods / sizeof v2_methods[0]; i++) {
        Mathrand2Rand *r =
            mathrand2_new(a, mathrand2_pcg_as_source(mathrand2_new_pcg(a, 1, 2)));
        for (Int repeat = 0; repeat < 20; repeat++, p++) {
            Got g;
            char arg[64];
            v2_call(r, a, v2_methods[i], repeat, &g, arg, sizeof arg);
            if (p >= rows)
                testing_t_fatalf_v(t, "r.%s(%s): missing golden value", v2_methods[i],
                                   arg);
            if (g.kind != -1)
                check_golden(t, &rand_v2_golden[p], v2_methods[i], arg, &g);
        }
    }
    CHECK_INT_EQ(p, rows);
    arena_free(&ar);
}

/* --------------------------------------------------------------- PCG */

static void TestPCG(TestingT *t) {
    Mathrand2PCG p;
    mathrand2_pcg_seed(&p, 1, 2);
    for (size_t i = 0; i < sizeof pcg_want / sizeof pcg_want[0]; i++) {
        uint64_t got = mathrand2_pcg_uint64(&p);
        if (got != pcg_want[i])
            testing_t_errorf_v(t, "#%d: got %#x, want %#x", (int)i, got, pcg_want[i]);
    }
}

static void TestPCGMarshal(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    static const Byte want[] =
        "pcg:\x12\x34\x56\x78\x9a\xbc\xde\xf0\xfe\xdc\xba\x98\x76\x54"
        "\x32\x10";
    Mathrand2PCG p;
    mathrand2_pcg_seed(&p, UINT64_C(0x123456789abcdef0), UINT64_C(0xfedcba9876543210));
    Error err;
    Slice data = mathrand2_pcg_marshal_binary(&p, a, &err);
    CHECK(BURROW_OK(err));
    CHECK(data.len == 20 && memcmp(data.p, want, 20) == 0);

    Slice b = slice_make(a, TYPE_BYTE, 4, 32);
    b = mathrand2_pcg_append_binary(&p, a, b, &err);
    CHECK(BURROW_OK(err));
    CHECK(b.len == 24 && memcmp((const Byte *)b.p + 4, want, 20) == 0);
    CHECK(memcmp(b.p, "\0\0\0\0", 4) == 0);

    Mathrand2PCG q = {0, 0};
    err = mathrand2_pcg_unmarshal_binary(
        &q, slice_from((void *)(uintptr_t)want, 20, 20, TYPE_BYTE));
    CHECK(BURROW_OK(err));
    CHECK(q.hi == p.hi && q.lo == p.lo);
    CHECK(mathrand2_pcg_uint64(&q) == mathrand2_pcg_uint64(&p));

    /* Not in Go's test: the ways an encoding can be wrong. */
    err = mathrand2_pcg_unmarshal_binary(
        &q, slice_from((void *)(uintptr_t)want, 19, 19, TYPE_BYTE));
    CHECK(BURROW_FAILED(err));
    if (BURROW_FAILED(err))
        CHECK(str_eq(error_text(err), BURROW_S("invalid PCG encoding")));
    Byte bad[20];
    memcpy(bad, want, 20);
    bad[0] = 'P';
    err = mathrand2_pcg_unmarshal_binary(&q, slice_from(bad, 20, 20, TYPE_BYTE));
    CHECK(BURROW_FAILED(err));
    arena_free(&ar);
}

/* ----------------------------------------------------------- ChaCha8 */

#define CHACHA8_N ((Int)(sizeof chacha8_output / sizeof chacha8_output[0]))

static void TestChaCha8(TestingT *t) {
    Mathrand2ChaCha8 p;
    mathrand2_cha_cha8_seed(&p, chacha8_seed);
    for (int round = 0; round < 2; round++) {
        for (Int i = 0; i < CHACHA8_N; i++) {
            uint64_t u = mathrand2_cha_cha8_uint64(&p);
            if (u != chacha8_output[i])
                testing_t_errorf_v(t, "ChaCha8 #%d = %#x, want %#x", i, u,
                                   chacha8_output[i]);
        }
        mathrand2_cha_cha8_seed(&p, chacha8_seed);
    }
}

static bool hash_is(Slice buf) {
    Sha256Sum256Ret s = sha256_sum256(buf);
    char hex[65];
    for (int i = 0; i < 32; i++)
        snprintf(hex + 2 * i, 3, "%02x", s.a[i]);
    return strcmp(hex, chacha8_hash) == 0;
}

static void TestChaCha8Read(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Mathrand2ChaCha8 *p = mathrand2_new_cha_cha8(a, chacha8_seed);
    Slice buf = slice_make(a, TYPE_BYTE, CHACHA8_OUTLEN, CHACHA8_OUTLEN);
    Error err;
    Int n = mathrand2_cha_cha8_read(p, buf, &err);
    CHECK(BURROW_OK(err));
    CHECK_INT_EQ(n, CHACHA8_OUTLEN);
    if (!hash_is(buf))
        testing_t_errorf_v(t, "transcript incorrect");

    /* One byte at a time. */
    mathrand2_cha_cha8_seed(p, chacha8_seed);
    Slice buf2 = slice_make(a, TYPE_BYTE, CHACHA8_OUTLEN, CHACHA8_OUTLEN);
    for (Int i = 0; i < CHACHA8_OUTLEN; i++) {
        n = mathrand2_cha_cha8_read(p, slice_sub(buf2, i, i + 1), &err);
        CHECK(n == 1 && BURROW_OK(err));
    }
    if (!hash_is(buf2))
        testing_t_errorf_v(t, "transcript incorrect (one byte reads)");

    mathrand2_cha_cha8_seed(p, chacha8_seed);
    n = mathrand2_cha_cha8_read(p, slice_make(a, TYPE_BYTE, 0, 0), &err);
    CHECK(n == 0 && BURROW_OK(err));

    /* Reads of random sizes, with a marshal and unmarshal now and then,
     * sometimes into a fresh generator. */
    Slice all = slice_make(a, TYPE_BYTE, CHACHA8_OUTLEN, CHACHA8_OUTLEN);
    Int done = 0;
    while (done < CHACHA8_OUTLEN) {
        if (mathrand2_int_n(2) == 0) {
            Slice out = mathrand2_cha_cha8_marshal_binary(p, a, &err);
            CHECK(BURROW_OK(err));
            if (mathrand2_int_n(2) == 0) {
                static const Byte zero[32];
                p = mathrand2_new_cha_cha8(a, zero);
            }
            err = mathrand2_cha_cha8_unmarshal_binary(p, out);
            if (BURROW_FAILED(err))
                testing_t_fatalf_v(t, "UnmarshalBinary: %v", err);
        }
        Int k = mathrand2_int_n(100);
        if (done + k > CHACHA8_OUTLEN)
            k = CHACHA8_OUTLEN - done;
        n = mathrand2_cha_cha8_read(p, slice_sub(all, done, done + k), &err);
        CHECK(n == k && BURROW_OK(err));
        done += k;
    }
    if (!hash_is(all))
        testing_t_errorf_v(t, "transcript incorrect (random reads)");
    arena_free(&ar);
}

static void check_enc(TestingT *t, const char *what, Int i, Slice got,
                      const RandEnc *want) {
    if (got.len != want->n || memcmp(got.p, want->p, (size_t)want->n) != 0)
        testing_t_errorf_v(t, "#%d: %s gave %d bytes, want %d", i, what, got.len,
                           want->n);
}

static void TestChaCha8Marshal(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Mathrand2ChaCha8 *p = mathrand2_new_cha_cha8(a, chacha8_seed);
    for (Int i = 0; i < CHACHA8_N; i++) {
        Error err;
        Slice enc = mathrand2_cha_cha8_marshal_binary(p, a, &err);
        CHECK(BURROW_OK(err));
        check_enc(t, "MarshalBinary", i, enc, &chacha8_marshal[i]);

        Slice b = slice_make(a, TYPE_BYTE, 4, 32);
        b = mathrand2_cha_cha8_append_binary(p, a, b, &err);
        CHECK(BURROW_OK(err));
        check_enc(t, "AppendBinary", i, slice_sub(b, 4, b.len), &chacha8_marshal[i]);

        memset(p, 0, sizeof *p);
        err = mathrand2_cha_cha8_unmarshal_binary(p, enc);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "#%d: UnmarshalBinary: %v", i, err);
        uint64_t u = mathrand2_cha_cha8_uint64(p);
        if (u != chacha8_output[i])
            testing_t_errorf_v(t, "ChaCha8 #%d = %#x, want %#x", i, u,
                               chacha8_output[i]);
    }
    arena_free(&ar);
}

static void TestChaCha8MarshalRead(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Mathrand2ChaCha8 *p = mathrand2_new_cha_cha8(a, chacha8_seed);
    Byte one[1];
    for (Int i = 0; i < 50; i++) {
        Error err;
        Slice enc = mathrand2_cha_cha8_marshal_binary(p, a, &err);
        CHECK(BURROW_OK(err));
        check_enc(t, "MarshalBinary", i, enc, &chacha8_marshalread[i]);

        Slice b = slice_make(a, TYPE_BYTE, 4, 32);
        b = mathrand2_cha_cha8_append_binary(p, a, b, &err);
        CHECK(BURROW_OK(err));
        check_enc(t, "AppendBinary", i, slice_sub(b, 4, b.len),
                  &chacha8_marshalread[i]);

        memset(p, 0, sizeof *p);
        err = mathrand2_cha_cha8_unmarshal_binary(p, enc);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "#%d: UnmarshalBinary: %v", i, err);
        (void)mathrand2_cha_cha8_read(p, slice_from(one, 1, 1, TYPE_BYTE), NULL);
    }
    arena_free(&ar);
}

/* Not in Go's tests: the encodings UnmarshalBinary turns away. */
static void TestChaCha8UnmarshalErrors(TestingT *t) {
    Mathrand2ChaCha8 p;
    mathrand2_cha_cha8_seed(&p, chacha8_seed);
    const RandEnc *good = &chacha8_marshal[0];
    Byte buf[80];

    /* Too short, and a wrong prefix. */
    Error err = mathrand2_cha_cha8_unmarshal_binary(
        &p, slice_from((void *)(uintptr_t)good->p, 47, 47, TYPE_BYTE));
    CHECK(BURROW_FAILED(err) &&
          str_eq(error_text(err), BURROW_S("invalid ChaCha8 encoding")));
    memcpy(buf, good->p, 48);
    buf[0] = 'C';
    err = mathrand2_cha_cha8_unmarshal_binary(&p, slice_from(buf, 48, 48, TYPE_BYTE));
    CHECK(BURROW_FAILED(err));

    /* A used count past the last word before the reseed. */
    memcpy(buf, good->p, 48);
    buf[15] = 125;
    err = mathrand2_cha_cha8_unmarshal_binary(&p, slice_from(buf, 48, 48, TYPE_BYTE));
    CHECK(BURROW_FAILED(err));
    buf[15] = 124;
    err = mathrand2_cha_cha8_unmarshal_binary(&p, slice_from(buf, 48, 48, TYPE_BYTE));
    CHECK(BURROW_OK(err));

    /* A read buffer that says it is longer than what follows. */
    memcpy(buf, "readbuf:\x05\x01\x02", 11);
    err = mathrand2_cha_cha8_unmarshal_binary(&p, slice_from(buf, 11, 11, TYPE_BYTE));
    CHECK(BURROW_FAILED(err) &&
          str_eq(error_text(err), BURROW_S("invalid ChaCha8 Read buffer encoding")));
    err = mathrand2_cha_cha8_unmarshal_binary(&p, slice_from(buf, 8, 8, TYPE_BYTE));
    CHECK(BURROW_FAILED(err));
}

/* ------------------------------------------------------------- panics */

static char panic_buf[256];

static Str recovered(Func f) {
    volatile Int n = 0;
    BURROW_TRY {
        BURROW_CALLF0(f);
    }
    BURROW_CATCH(r) {
        Str s = panic_text(r);
        n = s.len < (Int)sizeof panic_buf ? s.len : (Int)sizeof panic_buf;
        memcpy(panic_buf, s.p, (size_t)n);
    }
    BURROW_TRY_END;
    return str_from_bytes((const Byte *)panic_buf, n);
}

static void p_int64_n(void *env) {
    (void)env;
    (void)mathrand2_int64_n(0);
}

static void p_uint64_n(void *env) {
    (void)env;
    (void)mathrand2_uint64_n(0);
}

static void p_int32_n(void *env) {
    (void)env;
    (void)mathrand2_int32_n(-1);
}

static void p_uint32_n(void *env) {
    (void)env;
    (void)mathrand2_uint32_n(0);
}

static void p_int_n(void *env) {
    (void)env;
    (void)mathrand2_int_n(0);
}

static void p_uint_n(void *env) {
    (void)env;
    (void)mathrand2_uint_n(0);
}

static void p_n(void *env) {
    (void)env;
    (void)mathrand2_n((int64_t)0);
}

static void p_n_unsigned(void *env) {
    (void)env;
    (void)mathrand2_n((uint32_t)0);
}

static void no_swap(void *env, Int i, Int j) {
    (void)env;
    (void)i;
    (void)j;
}

static void p_shuffle2(void *env) {
    (void)env;
    mathrand2_shuffle(-1, BURROW_FN(SwapFunc, no_swap, NULL));
}

static void p_zipf2(void *env) {
    (void)env;
    (void)mathrand2_zipf_uint64(NULL);
}

static void p_int63n(void *env) {
    (void)env;
    (void)math_rand_int63n(0);
}

static void p_int31n(void *env) {
    (void)env;
    (void)math_rand_int31n(-5);
}

static void p_intn(void *env) {
    (void)env;
    (void)math_rand_intn(0);
}

static void p_shuffle1(void *env) {
    (void)env;
    math_rand_shuffle(-1, BURROW_FN(SwapFunc, no_swap, NULL));
}

static void p_zipf1(void *env) {
    (void)env;
    (void)math_rand_zipf_uint64(NULL);
}

static void p_readbuf(void *env) {
    Mathrand2ChaCha8 *p = (Mathrand2ChaCha8 *)env;
    Byte buf[64] = "readbuf:\x09";
    (void)mathrand2_cha_cha8_unmarshal_binary(p, slice_from(buf, 64, 64, TYPE_BYTE));
}

static void p_readbuf255(void *env) {
    Mathrand2ChaCha8 *p = (Mathrand2ChaCha8 *)env;
    Byte buf[64] = "readbuf:\xff";
    (void)mathrand2_cha_cha8_unmarshal_binary(p, slice_from(buf, 64, 64, TYPE_BYTE));
}

static void TestPanics(TestingT *t) {
    static const struct {
        void (*f)(void *);
        const char *want;
    } cases[] = {
        {p_int64_n, "invalid argument to Int64N"},
        {p_uint64_n, "invalid argument to Uint64N"},
        {p_int32_n, "invalid argument to Int32N"},
        {p_uint32_n, "invalid argument to Uint32N"},
        {p_int_n, "invalid argument to IntN"},
        {p_uint_n, "invalid argument to UintN"},
        {p_n, "invalid argument to N"},
        {p_n_unsigned, "invalid argument to N"},
        {p_shuffle2, "invalid argument to Shuffle"},
        {p_zipf2, "rand: nil Zipf"},
        {p_int63n, "invalid argument to Int63n"},
        {p_int31n, "invalid argument to Int31n"},
        {p_intn, "invalid argument to Intn"},
        {p_shuffle1, "invalid argument to Shuffle"},
        {p_zipf1, "rand: nil Zipf"},
        {p_readbuf, "runtime error: slice bounds out of range [-1:]"},
        {p_readbuf255, "runtime error: slice bounds out of range [1:0]"},
    };
    Mathrand2ChaCha8 p;
    mathrand2_cha_cha8_seed(&p, chacha8_seed);
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        Str got = recovered(BURROW_FN(Func, cases[i].f, &p));
        if (!str_eq(got, str_from_cstr(cases[i].want)))
            testing_t_errorf_v(t, "#%d: panic %q, want %q", (int)i, got, cases[i].want);
    }
}

/* --------------------------------------------------------- v1 misc */

static void TestFloat32(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    /* Go's issue 6721 showed up after 7533753 calls, so Go checks ten
     * million, and a hundredth of that in short mode. */
    Int num = testing_short() ? 100000 : 10000000;
    MathRandRand *r = math_rand_new(a, math_rand_new_source(a, 1));
    for (Int ct = 0; ct < num; ct++) {
        float f = math_rand_rand_float32(r);
        if (f >= 1)
            testing_t_fatalf_v(t, "Float32() should be in range [0,1). ct: %d f: %v",
                               ct, (double)f);
    }
    arena_free(&ar);
}

static void TestReadEmpty(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    MathRandRand *r = math_rand_new(a, math_rand_new_source(a, 1));
    Error err;
    Int n = math_rand_rand_read(r, slice_nil(TYPE_BYTE), &err);
    CHECK(n == 0 && BURROW_OK(err));
    arena_free(&ar);
}

static void TestReadByOneByte(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    MathRandRand *r = math_rand_new(a, math_rand_new_source(a, 1));
    Byte b1[100], b2[100];
    for (int i = 0; i < 100; i++)
        CHECK(math_rand_rand_read(r, slice_from(b1 + i, 1, 1, TYPE_BYTE), NULL) == 1);
    r = math_rand_new(a, math_rand_new_source(a, 1));
    CHECK(math_rand_rand_read(r, slice_from(b2, 100, 100, TYPE_BYTE), NULL) == 100);
    CHECK(memcmp(b1, b2, 100) == 0);
    arena_free(&ar);
}

static void TestReadSeedReset(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    MathRandRand *r = math_rand_new(a, math_rand_new_source(a, 42));
    Byte b1[128], b2[128];
    math_rand_rand_read(r, slice_from(b1, 128, 128, TYPE_BYTE), NULL);
    math_rand_rand_seed(r, 42);
    math_rand_rand_read(r, slice_from(b2, 128, 128, TYPE_BYTE), NULL);
    CHECK(memcmp(b1, b2, 128) == 0);
    arena_free(&ar);
}

static void swap_fatal(void *env, Int i, Int j) {
    TestingT *t = (TestingT *)env;
    testing_t_fatalf_v(t, "swap called, i=%d j=%d", i, j);
}

static void TestShuffleSmall(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    MathRandRand *r = math_rand_new(a, math_rand_new_source(a, 1));
    Mathrand2Rand *r2 =
        mathrand2_new(a, mathrand2_pcg_as_source(mathrand2_new_pcg(a, 1, 2)));
    for (Int n = 0; n <= 1; n++) {
        math_rand_rand_shuffle(r, n, BURROW_FN(SwapFunc, swap_fatal, t));
        mathrand2_rand_shuffle(r2, n, BURROW_FN(SwapFunc, swap_fatal, t));
    }
    arena_free(&ar);
}

static void TestSeedNop(TestingT *t) {
    /* randseednop=0: the global Seed takes effect. */
    burrow__math_rand_godebug_set("randseednop=0");
    math_rand_seed(1);
    int64_t before = math_rand_int63();
    math_rand_seed(1);
    int64_t after = math_rand_int63();
    if (before != after)
        testing_t_errorf_v(t, "global Seed should take effect");

    burrow__math_rand_godebug_set("randseednop=1");
    math_rand_seed(1);
    before = math_rand_int63();
    math_rand_seed(1);
    after = math_rand_int63();
    if (before == after)
        testing_t_errorf_v(t, "global Seed should be a no-op");

    burrow__math_rand_godebug_set("");
    math_rand_seed(1);
    before = math_rand_int63();
    math_rand_seed(1);
    after = math_rand_int63();
    if (before == after)
        testing_t_errorf_v(t, "global Seed should default to being a no-op");
    burrow__math_rand_godebug_set(NULL);
}

/* Not in Go's tests in this form: randautoseed=0 starts the shared generator
 * seeded with 1, which is what Go 1.19 and earlier did. */
static void TestAutoSeedOff(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    burrow__math_rand_godebug_set("randautoseed=0");
    MathRandRand *r = math_rand_new(a, math_rand_new_source(a, 1));
    for (int i = 0; i < 10; i++)
        CHECK(math_rand_int63() == math_rand_rand_int63(r));
    Byte b1[20], b2[20];
    math_rand_read(slice_from(b1, 20, 20, TYPE_BYTE), NULL);
    math_rand_rand_read(r, slice_from(b2, 20, 20, TYPE_BYTE), NULL);
    CHECK(memcmp(b1, b2, 20) == 0);
    burrow__math_rand_godebug_set(NULL);
    arena_free(&ar);
}

static void TestAuto(TestingT *t) {
    burrow__math_rand_godebug_set("randseednop=0");
    int64_t out[10];
    for (int i = 0; i < 10; i++)
        out[i] = math_rand_int63();
    math_rand_seed(1);
    int found = 0;
    for (int i = 0; i < 1000; i++) {
        if (math_rand_int63() == out[found]) {
            found++;
            if (found == 10)
                testing_t_fatalf_v(t, "found unseeded output in Seed(1) output");
        }
    }
    burrow__math_rand_godebug_set(NULL);
}

/* ---------------------------------------------------- user sources */

/* A source written outside the package, with a Uint64 in its method set so
 * that Rand finds it, and a counter of how often each was called. */
typedef struct CountSource {
    uint64_t x;
    int int63s;
    int uint64s;
} CountSource;

static int64_t count_int63(void *self) {
    CountSource *s = (CountSource *)self;
    s->int63s++;
    return (int64_t)(++s->x & (uint64_t)INT64_MAX);
}

static void count_seed(void *self, int64_t seed) {
    ((CountSource *)self)->x = (uint64_t)seed;
}

static uint64_t count_uint64(CountSource *s) {
    s->uint64s++;
    return ++s->x;
}

#define COUNT_METHODS(M, T) M(T, Uint64, count_uint64, MATH_RAND_SIG_UINT64)
BURROW_METHODS_DEFINE(CountSource, COUNT_METHODS);

static const Type count_type = {
    {(const Byte *)"countSource", 11},
    {(const Byte *)"rand_test", 9},
    KIND_STRUCT,
    (uint32_t)sizeof(CountSource),
    (uint16_t)_Alignof(CountSource),
    0,
    1,
    NULL,
    burrow__methods_CountSource,
    NULL,
    NULL,
    0,
    0x636e7473U,
    NULL,
};

static const MathRandSourceVT count_vt = {&count_type, count_int63, count_seed};

static void TestSource64(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    CountSource cs = {0, 0, 0};
    MathRandSource src = {&count_vt, &cs};
    MathRandRand *r = math_rand_new(a, src);
    CHECK(math_rand_rand_uint64(r) == 1);
    CHECK(cs.uint64s == 1 && cs.int63s == 0);
    CHECK(math_rand_rand_int63(r) == 2);
    CHECK(cs.int63s == 1);

    /* The package's own source is a Source64 as well. */
    MathRandRand *r1 = math_rand_new(a, math_rand_new_source(a, 7));
    CHECK(r1->s64.vt != NULL);
    CHECK(r1->u64 == NULL);
    arena_free(&ar);
}

/* ------------------------------------------------------------- Zipf */

static void TestZipf(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Mathrand2Rand *r =
        mathrand2_new(a, mathrand2_pcg_as_source(mathrand2_new_pcg(a, 1, 2)));
    CHECK(mathrand2_new_zipf(a, r, 1.0, 1, 10) == NULL);
    CHECK(mathrand2_new_zipf(a, r, 2.0, 0.5, 10) == NULL);
    Mathrand2Zipf *z = mathrand2_new_zipf(a, r, 1.5, 2, 100);
    CHECK(z != NULL);
    Int counts[101] = {0};
    for (int i = 0; i < 10000; i++) {
        uint64_t v = mathrand2_zipf_uint64(z);
        if (v > 100)
            testing_t_fatalf_v(t, "Zipf gave %d, past imax", (int64_t)v);
        counts[v]++;
    }
    /* The distribution falls away from zero. */
    CHECK(counts[0] > counts[1] && counts[1] > counts[5] && counts[5] > counts[50]);

    MathRandRand *r1 = math_rand_new(a, math_rand_new_source(a, 1));
    CHECK(math_rand_new_zipf(a, r1, 0.5, 1, 10) == NULL);
    MathRandZipf *z1 = math_rand_new_zipf(a, r1, 1.1, 1, 1000);
    CHECK(z1 != NULL);
    for (int i = 0; i < 1000; i++)
        CHECK(math_rand_zipf_uint64(z1) <= 1000);
    arena_free(&ar);
}

/* ------------------------------------------------------ v2 top level */

static void TestN(TestingT *t) {
    for (int i = 0; i < 1000; i++) {
        int64_t v = mathrand2_n((int64_t)10);
        if (v < 0 || v >= 10)
            testing_t_fatalf_v(t, "N(10) returned %d", v);
        uint8_t u = (uint8_t)mathrand2_n((uint8_t)3);
        if (u >= 3)
            testing_t_fatalf_v(t, "N(uint8(3)) returned %d", u);
    }
}

static void TestGlobalRanges(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (int i = 0; i < 1000; i++) {
        CHECK(mathrand2_int64() >= 0);
        CHECK(mathrand2_int32() >= 0);
        CHECK(mathrand2_int() >= 0);
        double f = mathrand2_float64();
        CHECK(f >= 0 && f < 1);
        float g = mathrand2_float32();
        CHECK(g >= 0 && g < 1);
        CHECK(mathrand2_int_n(7) < 7);
        CHECK(mathrand2_uint32_n(1) == 0);
        CHECK(math_rand_int63() >= 0);
        CHECK(math_rand_int31() >= 0);
        CHECK(math_rand_intn(9) < 9);
        f = math_rand_float64();
        CHECK(f >= 0 && f < 1);
        CHECK(mathrand2_exp_float64() >= 0);
        CHECK(math_rand_exp_float64() >= 0);
    }
    Slice p = mathrand2_perm(a, 50);
    Slice q = math_rand_perm(a, 50);
    bool seen[50] = {false}, seen1[50] = {false};
    for (Int i = 0; i < 50; i++) {
        seen[((const Int *)p.p)[i]] = true;
        seen1[((const Int *)q.p)[i]] = true;
    }
    for (int i = 0; i < 50; i++)
        CHECK(seen[i] && seen1[i]);
    arena_free(&ar);
}

/* Both shared generators from many goroutines at once, for the race
 * detector and the sanitizers. Go's race_test.go does the same. */
static void concurrent_worker(void *env) {
    (void)env;
    Byte buf[16];
    for (int i = 0; i < 2000; i++) {
        (void)mathrand2_uint64();
        (void)mathrand2_int_n(1000);
        (void)mathrand2_norm_float64();
        (void)math_rand_int63();
        (void)math_rand_uint64();
        (void)math_rand_intn(1000);
        math_rand_read(slice_from(buf, 16, 16, TYPE_BYTE), NULL);
    }
}

static void TestConcurrent(TestingT *t) {
    (void)t;
    SyncWaitGroup wg;
    memset(&wg, 0, sizeof wg);
    for (int i = 0; i < 8; i++)
        sync_wait_group_go(&wg, BURROW_FN(Func, concurrent_worker, NULL));
    sync_wait_group_wait(&wg);
}

#define TESTS(X)                                                                       \
    X(TestRegress)                                                                     \
    X(TestRegressV2)                                                                   \
    X(TestPCG)                                                                         \
    X(TestPCGMarshal)                                                                  \
    X(TestChaCha8)                                                                     \
    X(TestChaCha8Read)                                                                 \
    X(TestChaCha8Marshal)                                                              \
    X(TestChaCha8MarshalRead)                                                          \
    X(TestChaCha8UnmarshalErrors)                                                      \
    X(TestPanics)                                                                      \
    X(TestFloat32)                                                                     \
    X(TestReadEmpty)                                                                   \
    X(TestReadByOneByte)                                                               \
    X(TestReadSeedReset)                                                               \
    X(TestShuffleSmall)                                                                \
    X(TestSeedNop)                                                                     \
    X(TestAutoSeedOff)                                                                 \
    X(TestAuto)                                                                        \
    X(TestSource64)                                                                    \
    X(TestZipf)                                                                        \
    X(TestN)                                                                           \
    X(TestGlobalRanges)                                                                \
    X(TestConcurrent)

TESTING_MAIN(TESTS)
