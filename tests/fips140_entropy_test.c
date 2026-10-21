/* Derived from Go's src/crypto/internal/fips140test/entropy_test.go.
 * Go source: go1.27.1.
 *
 * Not in Go: the test programs take only the -test.* flags, so there are no
 * -entropy-samples and -nist-sp800-90b flags. TestEntropySamples collects the
 * samples Go's does, but never writes them out or runs the NIST tools on them,
 * which is what those flags are for.
 *
 * Copyright 2025 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"
#include "fipstest.h"

#include "burrow/burrow.h"
#include "burrow/crypto/rand.h"
#include "burrow/crypto/sha256.h"
#include "burrow/crypto/sha512.h"
#include "burrow/encoding/hex.h"

#include "../src/crypto/drbg_internal.h"
#include "../src/crypto/entropy_internal.h"

#include <stdint.h>
#include <string.h>

#define S(x) BURROW_S(x)

static EntropyScratchBuffer memory;

/* samplesOrTryAgain calls entropy.Samples up to 10 times until it succeeds.
 * Samples has a non-negligible chance of failing the health tests, as required
 * by SP 800-90B. */
static void samples_or_try_again(TestingT *t, Slice samples) {
    testing_t_helper(t);
    for (int i = 0; i < 10; i++) {
        Error err = burrow__entropy_samples(samples, &memory);
        if (BURROW_FAILED(err)) {
            testing_t_logf_v(t, "entropy.Samples() failed: %s", error_text(err));
            continue;
        }
        return;
    }
    testing_t_fatal_v(t, "entropy.Samples() failed 10 times in a row");
}

static void TestEntropySamples(TestingT *t) {
    MUST_SUPPORT_FIPS140(t);
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    Int seq_sample_count = 1000000;
    Slice seq_samples = slice_make(a, TYPE_UINT8, seq_sample_count, seq_sample_count);
    samples_or_try_again(t, seq_samples);

    /* Not in Go: restartSamples is one block of 1000 by 1000. */
    Slice restart_samples = slice_make(a, TYPE_UINT8, 1000 * 1000, 1000 * 1000);
    for (Int i = 0; i < 1000; i++) {
        uint8_t samples[1024];
        samples_or_try_again(t, slice_from(samples, 1024, 1024, TYPE_UINT8));
        memcpy((uint8_t *)restart_samples.p + i * 1000, samples, 1000);
    }
    arena_free(&ar);
}

static void TestEntropySHA384(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    Byte input[1024];
    for (int i = 0; i < 1024; i++)
        input[i] = (Byte)i;
    Sha512Sum384Ret want = sha512_sum384(slice_from(input, 1024, 1024, TYPE_BYTE));
    Byte got[48];
    burrow__entropy_sha384(got, input);
    if (memcmp(got, want.a, 48) != 0) {
        Slice gs = slice_from(got, 48, 48, TYPE_BYTE);
        Slice ws = slice_from(want.a, 48, 48, TYPE_BYTE);
        testing_t_errorf_v(t, "SHA384() = %x, want %x", gs, ws);
    }

    for (Int l = 0; l < 1024 * 3 + 1; l++) {
        Slice in = slice_make(a, TYPE_BYTE, l, l);
        (void)crypto_rand_read(in, NULL);
        Sha512Sum384Ret w = sha512_sum384(in);
        Byte g[48];
        burrow__entropy_testing_only_sha384(g, in);
        if (memcmp(g, w.a, 48) != 0) {
            Slice gs = slice_from(g, 48, 48, TYPE_BYTE);
            Slice ws = slice_from(w.a, 48, 48, TYPE_BYTE);
            testing_t_errorf_v(t, "TestingOnlySHA384(%d bytes) = %x, want %x", l, gs,
                               ws);
        }
    }
    arena_free(&ar);
}

/* Not in Go: bytes.Repeat of one byte, appended to b[*n:]. */
static void put(uint8_t *b, Int *n, uint8_t v, Int count) {
    memset(b + *n, v, (size_t)count);
    *n += count;
}

static Slice samples_of(uint8_t *b, Int n) {
    return slice_from(b, n, n, TYPE_UINT8);
}

static void TestEntropyRepetitionCountTest(TestingT *t) {
    uint8_t good[41 * 100];
    Int n = 0;
    for (int i = 0; i < 100; i++) {
        put(good, &n, 42, 40);
        put(good, &n, 1, 1);
    }
    Error err = burrow__entropy_repetition_count_test(samples_of(good, n));
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "RepetitionCountTest(good) = %s, want nil",
                           error_text(err));

    uint8_t bad[40 + 40 + 41 + 40];
    n = 0;
    put(bad, &n, 0, 40);
    put(bad, &n, 1, 40);
    put(bad, &n, 42, 41);
    put(bad, &n, 2, 40);
    if (!BURROW_FAILED(burrow__entropy_repetition_count_test(samples_of(bad, n))))
        testing_t_error_v(t, "RepetitionCountTest(bad) = nil, want error");

    n = 0;
    put(bad, &n, 42, 41);
    if (!BURROW_FAILED(burrow__entropy_repetition_count_test(samples_of(bad, n))))
        testing_t_error_v(t, "RepetitionCountTest(bad) = nil, want error");
}

static void TestEntropyAdaptiveProportionTest(TestingT *t) {
    uint8_t good[409 + (512 - 409) + 409];
    Int n = 0;
    put(good, &n, 0, 409);
    put(good, &n, 1, 512 - 409);
    put(good, &n, 0, 409);
    Error err = burrow__entropy_adaptive_proportion_test(samples_of(good, n));
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "AdaptiveProportionTest(good) = %s, want nil",
                           error_text(err));

    /* These fall out of the window. */
    uint8_t bad[100 + 6 * 100 + 410];
    n = 0;
    put(bad, &n, 1, 100);
    for (int i = 0; i < 100; i++)
        for (uint8_t v = 1; v <= 6; v++)
            put(bad, &n, v, 1);
    /* These are in the window. */
    put(bad, &n, 42, 410);
    err = burrow__entropy_adaptive_proportion_test(samples_of(bad, n - 1));
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "AdaptiveProportionTest(bad[:len(bad)-1]) = %s, want nil",
                           error_text(err));
    if (!BURROW_FAILED(burrow__entropy_adaptive_proportion_test(samples_of(bad, n))))
        testing_t_error_v(t, "AdaptiveProportionTest(bad) = nil, want error");
}

static void free_arena(void *env) {
    arena_free(env);
    mem_free(heap_allocator(), env, sizeof(Arena), _Alignof(Arena));
}

static void TestEntropyUnchanged(TestingT *t) {
    /* Not in Go: testenv.MustHaveSource, and the files. The tests run from the
     * top of the tree, and burrow's entropy source is these two files, so they
     * are what is hashed, and the hash is burrow's. .gitattributes keeps a
     * Windows checkout from changing their line endings. */
    static const char *const files[] = {"entropy.c", "entropy_internal.h"};
    Str dir = S("src/crypto");

    Arena *ar = BURROW_NEW(heap_allocator(), Arena);
    if (ar == NULL)
        testing_t_fatal_v(t, "out of memory");
    arena_init(ar, NULL, 0);
    testing_t_cleanup(t, BURROW_FN(Func, free_arena, ar));
    Alloc *a = arena_allocator(ar);

    Slice h = slice_nil(TYPE_BYTE);
    for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
        Str path = str_from_cstr(files[i]);
        Error err = BURROW_NO_ERROR;
        Slice data = os_read_file(a, filepath_join_v(a, 2, dir, path), &err);
        if (BURROW_FAILED(err)) {
            if (errors_is(err, fs_err_not_exist))
                testing_t_skipf_v(t, "no sources in %s", dir);
            testing_t_fatalf_v(t, "WalkDir: %s", error_text(err));
        }
        testing_t_logf_v(t, "Hashing %s (%d bytes)", path, data.len);
        Str line = fmt_sprintf_v(a, "%s %d\n", path, data.len);
        h = slice_append_slice(
            a, h, slice_from((void *)(uintptr_t)line.p, line.len, line.len, TYPE_BYTE));
        h = slice_append_slice(a, h, data);
    }

    /* The crypto/internal/entropy/v1.0.0 package is certified as a FIPS 140-3
     * entropy source through the Entropy Source Validation program,
     * independently of the FIPS 140-3 module. It must not change even across
     * FIPS 140-3 module versions, in order to reuse the ESV certificate. */
    Str exp = S("06f37e0faba5a3e526cb608bad75e96eeca8bd54ae5c55b7eef4860944ae9821");
    Sha256Sum256Ret sum = sha256_sum256(h);
    Str got = hex_encode_to_string(a, slice_from(sum.a, 32, 32, TYPE_BYTE));
    if (!str_eq(got, exp))
        testing_t_errorf_v(t, "hash of crypto/internal/entropy/v1.0.0 = %s, want %s",
                           got, exp);
}

static void race_seed(void *env) {
    (void)env;
    Byte seed[48];
    (void)burrow__entropy_seed(seed, &memory);
}

static void race_read(void *env) {
    (void)env;
    Byte b[64];
    burrow__drbg_read(slice_from(b, 64, 64, TYPE_BYTE));
}

static void TestEntropyRace(TestingT *t) {
    (void)t;
    /* Check that concurrent calls to Seed don't trigger the race detector. */
    for (int i = 0; i < 16; i++)
        go(BURROW_FN(Func, race_seed, NULL));
    /* Same, with the higher-level DRBG. */
    for (int i = 0; i < 16; i++)
        go(BURROW_FN(Func, race_read, NULL));
}

#define TESTS(X)                                                                       \
    X(TestEntropySamples)                                                              \
    X(TestEntropySHA384)                                                               \
    X(TestEntropyRepetitionCountTest)                                                  \
    X(TestEntropyAdaptiveProportionTest)                                               \
    X(TestEntropyUnchanged)                                                            \
    X(TestEntropyRace)

TESTING_MAIN(TESTS)
