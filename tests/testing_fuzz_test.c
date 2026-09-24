/* The pieces of the fuzzing engine, checked against Go's internal/fuzz: the
 * values below are what Go's own pcgRand, mutator and minimizeBytes give for
 * the same seed, printed by a program built from their sources.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "../src/testing/fuzz.h"

#include "burrow/testing.h"

#include "burrow/core.h"
#include "burrow/fmt.h"
#include "burrow/iface.h"
#include "burrow/type.h"

#include <stdio.h>
#include <string.h>

/* newPcgRand for GODEBUG=fuzzseed=seed, as the n-th generator the process
 * made, which is what decides its stream. */
static burrow__FuzzRand seeded(uint64_t seed, uint64_t n) {
    const uint64_t mul = 6364136223846793005u;
    burrow__FuzzRand r = {seed, (n << 1) | 1};
    r.state = r.state * mul + r.inc;
    r.state += seed;
    r.state = r.state * mul + r.inc;
    return r;
}

static void TestRand(TestingT *t) {
    burrow__FuzzRand r = seeded(42, 1);
    static const uint32_t want[] = {3189644333u, 1816033613u, 2369029826u, 2024868014u,
                                    3009602134u};
    for (size_t i = 0; i < sizeof want / sizeof want[0]; i++) {
        uint32_t got = burrow__fuzz_rand_uint32(&r);
        if (got != want[i])
            testing_t_errorf_v(t, "uint32 #%d = %d, want %d", (Int)i, (int64_t)got,
                               (int64_t)want[i]);
    }
    uint32_t a = burrow__fuzz_rand_uint32n(&r, 10);
    Int b = burrow__fuzz_rand_intn(&r, 1000);
    bool c = burrow__fuzz_rand_bool(&r);
    uint32_t d = burrow__fuzz_rand_uint32n(&r, 7);
    if (a != 7 || b != 967 || c || d != 4)
        testing_t_errorf_v(
            t, "uint32n, intn, bool, uint32n = %d %d %v %d, want 7 967 false 4",
            (int64_t)a, b, c, (int64_t)d);
}

typedef struct MutateStep {
    const char *bytes;
    Int nbytes;
    const char *str;
    Int nstr;
    int64_t i64;
    double f64;
    bool b;
    uint8_t u8;
    int16_t i16;
    float f32;
    uint32_t u32;
} MutateStep;

/* Forty calls of mutate on the same values, each one starting from what the
 * one before left. A []byte shares the mutator's scratch space with the
 * string, so a change to the string shows up in the bytes as well, in Go
 * and here. */
static const MutateStep mutate_steps[] = {
    {"hello, world", 12, "str", 3, -5, 53.5, true, 9, 300, 1.25, 7},
    {"hello, world", 12, "str", 3, -5, 53.5, true, 1, 300, 1.25, 7},
    {"hello, world", 12, "sTr", 3, -5, 53.5, true, 1, 300, 1.25, 7},
    {"hello, wo\x96"
     "\x96"
     "\x96"
     "",
     12, "sTr", 3, -5, 53.5, true, 1, 300, 1.25, 7},
    {"hello, wo\x96"
     "\x96"
     "\x96"
     "",
     12, "sTr", 3, -5, 53.5, true, 1, 300, 1.25, 6},
    {"hello, wo\x96"
     "\x96"
     "\x96"
     "",
     12, "sTr", 3, -5, 148.5, true, 1, 300, 1.25, 6},
    {"hello,UUU\x96"
     "\x96"
     "\x96"
     "",
     12, "sTr", 3, -5, 148.5, true, 1, 300, 1.25, 6},
    {"hello,UUU\x96"
     "\x96"
     "\x96"
     "",
     12, "sTr", 3, -5, 148.5, true, 1, 376, 1.25, 6},
    {"hello,UUU\x96"
     "\x96"
     "\x96"
     "",
     12, "sTr", 3, -5, 83.5, true, 1, 376, 1.25, 6},
    {"hello,UUU\x96"
     "\x96"
     "\x96"
     "",
     12, "sTr", 3, -5, 83.5, true, 1, 376, 10, 6},
    {"hello,UUU\x96"
     "\x96"
     "\x96"
     "",
     12, "sTr", 3, -5, 83.5, true, 1, 376, 10, 6},
    {"sT\x9f"
     "ro,UUU\x96"
     "\x96"
     "\x96"
     "",
     12,
     "sT\x9f"
     "r",
     4, -5, 83.5, true, 1, 376, 10, 6},
    {"sT\x9f"
     "ro,UUU\x96"
     "\x96"
     "\x96"
     "",
     12,
     "sT\x9f"
     "r",
     4, -5, 83.5, true, 1, 376, 10, 38},
    {"sT\x9f"
     "ro,UUU\x96"
     "\x96"
     "\x96"
     "",
     12,
     "sT\x9f"
     "r",
     4, -5, 83.5, true, 7, 376, 10, 38},
    {"sT\x9f"
     "ro,UUU\x96"
     "\x96"
     "\x96"
     "",
     12,
     "sT\x9f"
     "r",
     4, -5, 83.5, false, 7, 376, 10, 38},
    {"sT\x9f"
     "rU,UUU\x96"
     "\x96"
     "\x96"
     "",
     12,
     "sT\x9f"
     "r",
     4, -5, 83.5, false, 7, 376, 10, 38},
    {"sT\x9f"
     "rU,UUU\x96"
     "\x96"
     "\x96"
     "",
     12,
     "sT\x9f"
     "r",
     4, -5, 83.5, false, 7, 376, 10, 58},
    {"sT\x9f"
     "rU,UUU\x96"
     "\x96"
     "\x96"
     "",
     12,
     "sT\x9f"
     "r",
     4, -5, 83.5, false, 7, 376, 10, 59},
    {"sT\x9f"
     "rU,UUU\x96"
     "\x96"
     "\x96"
     "",
     12,
     "sT\x9f"
     "r",
     4, -5, 83.5, false, 7, 376, 10, 59},
    {"sT\x9f"
     "rU,UUU\x96"
     "\x96"
     "\x96"
     "",
     12,
     "sT\x9f"
     "r",
     4, -5, 83.5, false, 7, 381, 10, 59},
    {"sT\x9f"
     "rU,UUU\x96"
     "\x96"
     "\x96"
     "",
     12,
     "sT\x9f"
     "r",
     4, -5, 126.5, false, 7, 381, 10, 59},
    {"sT\x9f"
     "rU,UUU\x96"
     "\x96"
     "\x96"
     "",
     12,
     "sT\x9f"
     "r",
     4, -5, 126.5, false, 7, 381, 70, 59},
    {"sT\x9f"
     "rU,UUU\x96"
     "\x96"
     "\x96"
     "",
     12,
     "sT\x9f"
     "r",
     4, -5, 126.5, false, 7, 312, 70, 59},
    {"sT\x9f"
     "rU,UUU\x96"
     "\x96"
     "\x96"
     "",
     12,
     "sT\x9f"
     "r",
     4, -5, 83.5, false, 7, 312, 70, 59},
    {"sT\x9f"
     "rU,UUU\x96"
     "\x96"
     "\x96"
     "",
     12,
     "sT\x9f"
     "r",
     4, -5, 16.7, false, 7, 312, 70, 59},
    {"sT\x9f"
     "rU,UUU\x96"
     "\x96"
     "\x96"
     "",
     12,
     "sT\x9f"
     "r",
     4, 91, 16.7, false, 7, 312, 70, 59},
    {"sT\x9f"
     "rU,UUU\x96"
     "\x96"
     "\x96"
     "",
     12,
     "sT\x9f"
     "r",
     4, 91, 16.7, false, 26, 312, 70, 59},
    {"sT\x9f"
     "rU,UUU\x96"
     "\x96"
     "\x96"
     "",
     12,
     "sT\x9f"
     "r",
     4, 91, 16.7, false, 102, 312, 70, 59},
    {"\x01"
     "T\x9f"
     "rU,UUU\x96"
     "\x96"
     "\x96"
     "",
     12,
     "\x01"
     "T\x9f"
     "r",
     4, 91, 16.7, false, 102, 312, 70, 59},
    {"\x01"
     "T\x9f"
     "rU,UUU\x96"
     "\x96"
     "\x96"
     "",
     12,
     "\x01"
     "T\x9f"
     "r",
     4, 91, 16.7, false, 102, 312, 70, 124},
    {"\x01"
     "T\x9f"
     "rU,UUU\x96"
     "\x96"
     "\x96"
     "",
     12,
     "\x01"
     "T\x9f"
     "r",
     4, 91, 16.7, false, 102, 312, 70, 157},
    {"\xfa"
     "\x00"
     "\x00"
     "\xfa"
     "U,UUU\x96"
     "\x96"
     "\x96"
     "",
     12,
     "\x01"
     "T\x9f"
     "r",
     4, 91, 16.7, false, 102, 312, 70, 157},
    {"\xfa"
     "\x00"
     "\x00"
     "\xfa"
     "U,UUU\x96"
     "\x96"
     "\x96"
     "",
     12,
     "\x01"
     "T\x9f"
     "r",
     4, 91, 16.7, false, 102, 312, 70, 244},
    {"\xfa"
     "\x00"
     "\x00"
     "\xfa"
     "U\x96"
     "\x96"
     "",
     7,
     "\x01"
     "T\x9f"
     "r",
     4, 91, 16.7, false, 102, 312, 70, 244},
    {"\xfa"
     "\x00"
     "\x00"
     "\xfa"
     "U\x96"
     "\x96"
     "",
     7,
     "\x01"
     "T\x9f"
     "r",
     4, 91, 66.8, false, 102, 312, 70, 244},
    {"\xfa"
     "\x00"
     "\x00"
     "\xfa"
     "U\x96"
     "\x96"
     "",
     7,
     "\x01"
     "T\x9f"
     "r",
     4, 91, 63.8, false, 102, 312, 70, 244},
    {"\xfa"
     "\x00"
     "\x00"
     "\xfa"
     "U\x96"
     "\x96"
     "",
     7,
     "\x01"
     "T\x9f"
     "r",
     4, 91, 4.799999999999997, false, 102, 312, 70, 244},
    {"\xfa"
     "\x00"
     "\x00"
     "\xfa"
     "U\x96"
     "\x96"
     "",
     7,
     "\x01"
     "T\x9f"
     "r",
     4, 91, 4.799999999999997, false, 102, 312, 70, 244},
    {"\xfa"
     "\x00"
     "\x00"
     "\xfa"
     "U\x96"
     "\x96"
     "",
     7,
     "\x01"
     "T\x9f"
     "r",
     4, 91, 4.799999999999997, false, 81, 312, 70, 244},
    {"\xfa"
     "\x00"
     "\x00"
     "\xfa"
     "U\x96"
     "\x96"
     "",
     7,
     "\x01"
     "T\x9f"
     "r",
     4, 91, 4.799999999999997, false, 81, 312, 70, 221},
};

static void TestMutate(TestingT *t) {
    burrow__FuzzMutator m;
    burrow__fuzz_mutator_init(&m);
    m.r = seeded(42, 2);
    Byte raw[] = "hello, world";
    Slice bytes = slice_from(raw, 12, 12, TYPE_BYTE);
    Str str = BURROW_S("str");
    int64_t i64 = -5;
    double f64 = 3.5;
    bool b = true;
    uint8_t u8 = 9;
    int16_t i16 = 300;
    float f32 = 1.25f;
    uint32_t u32 = 7;
    Any vals[] = {
        BURROW_ANY(TYPE_BYTES, &bytes), BURROW_ANY(TYPE_STRING, &str),
        BURROW_ANY(TYPE_INT64, &i64),   BURROW_ANY(TYPE_FLOAT64, &f64),
        BURROW_ANY(TYPE_BOOL, &b),      BURROW_ANY(TYPE_UINT8, &u8),
        BURROW_ANY(TYPE_INT16, &i16),   BURROW_ANY(TYPE_FLOAT32, &f32),
        BURROW_ANY(TYPE_UINT32, &u32),
    };
    for (size_t i = 0; i < sizeof mutate_steps / sizeof mutate_steps[0]; i++) {
        const MutateStep *w = &mutate_steps[i];
        burrow__fuzz_mutate(&m, vals, 9, 4000);
        Str gb = str_from_bytes((const Byte *)bytes.p, bytes.len);
        Str wb = str_from_bytes((const Byte *)w->bytes, w->nbytes);
        Str ws = str_from_bytes((const Byte *)w->str, w->nstr);
        if (!str_eq(gb, wb) || !str_eq(str, ws) || i64 != w->i64 || f64 != w->f64 ||
            b != w->b || u8 != w->u8 || i16 != w->i16 || f32 != w->f32 ||
            u32 != w->u32) {
            testing_t_errorf_v(t,
                               "step %d: got %q %q %d %v %v %d %d %v %d\n"
                               "want %q %q %d %v %v %d %d %v %d",
                               (Int)i, gb, str, i64, f64, b, u8, i16, f32, u32, wb, ws,
                               w->i64, w->f64, w->b, w->u8, w->i16, w->f32, w->u32);
            break;
        }
    }
    burrow__fuzz_mutator_free(&m);
}

typedef struct MinimizeEnv {
    char last[32];
    Int nlast;
    int calls;
} MinimizeEnv;

static bool minimize_try(void *env, const Byte *p, Int n) {
    MinimizeEnv *e = (MinimizeEnv *)env;
    e->calls++;
    bool ok = n >= 3 && p[n - 1] == 'f' && p[0] == '0';
    if (ok) {
        memcpy(e->last, p, (size_t)n);
        e->nlast = n;
    }
    return ok;
}

static bool minimize_never(void *env) {
    (void)env;
    return false;
}

static void TestMinimizeBytes(TestingT *t) {
    Byte v[] = "0123456789abcdef";
    Int n = 16;
    MinimizeEnv env = {{0}, 0, 0};
    burrow__fuzz_minimize_bytes(v, &n, minimize_try, minimize_never, &env);
    Str got = str_from_bytes((const Byte *)env.last, env.nlast);
    if (!str_eq(got, BURROW_S("00f")) || env.calls != 56)
        testing_t_errorf_v(t, "smallest %q after %d tries, want \"00f\" after 56", got,
                           env.calls);
    if (!str_eq(str_from_bytes(v, n), BURROW_S("00f")))
        testing_t_errorf_v(t, "left %q in v, want \"00f\"", str_from_bytes(v, n));
}

static void TestSha256(TestingT *t) {
    static Byte big[768];
    for (size_t i = 0; i < sizeof big; i++)
        big[i] = (Byte)i;
    static Byte as[1000];
    memset(as, 'a', sizeof as);
    struct {
        const void *p;
        Int n;
        const char *want;
    } cases[] = {
        {"", 0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        {"abc", 3, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
        {as, 1000, "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3"},
        {big, 768, "f3a25aa93aa2fbba28d79260535bbd6a5eb0fc1c24a8b0f04e12b484c1dfe363"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        Byte sum[32];
        burrow__fuzz_sha256(cases[i].p, cases[i].n, sum);
        char hex[65];
        for (int k = 0; k < 32; k++)
            snprintf(hex + 2 * k, 3, "%02x", sum[k]);
        if (strcmp(hex, cases[i].want) != 0)
            testing_t_errorf_v(t, "case %d: %s, want %s", (Int)i, hex, cases[i].want);
    }
}

#define TESTS(X)                                                                       \
    X(TestRand)                                                                        \
    X(TestMutate)                                                                      \
    X(TestMinimizeBytes)                                                               \
    X(TestSha256)
TESTING_MAIN(TESTS)
