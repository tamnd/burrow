/* Derived from Go's src/testing/cryptotest/rand_test.go.
 * Go source: go1.27.1.
 *
 * Copyright 2025 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "../src/crypto/rand_internal.h"
#include "burrow/burrow.h"
#include "burrow/crypto/ecdsa.h"
#include "burrow/crypto/elliptic.h"
#include "burrow/crypto/mlkem.h"
#include "burrow/crypto/rand.h"
#include "burrow/encoding/hex.h"
#include "burrow/testing/cryptotest.h"

#include <stdint.h>
#include <string.h>

static const char seed1_hex[] =
    "6ae6783f4fbde91b6eb88b73a48ed247dbe5882e2579683432c1bfc525454add"
    "0cd87274d67084caaf0e0d36c8496db7fef55fe0e125750aa608d5e20ffc2d12";

/* Not in Go: the decoded seed1, the 64 byte buf the rand.Read and rand.Reader
 * subtests share, and what rand.Reader's cleanup and its reader of fives need
 * to outlive the subtest's function. */
typedef struct SgrEnv {
    Byte seed1[64];
    Byte buf[64];
    IoReader old_reader;
    Byte fives[64];
    BytesReader fives_reader;
} SgrEnv;

static Slice sgr_seed1(SgrEnv *e) {
    return slice_from(e->seed1, 64, 64, TYPE_BYTE);
}

static Slice sgr_buf(SgrEnv *e) {
    return slice_from(e->buf, 64, 64, TYPE_BYTE);
}

/* Not in Go: rand.Reader.Read(buf), one Read through the interface. */
static void reader_read(Slice b) {
    (void)crypto_rand_reader.vt->read(crypto_rand_reader.data, b, NULL);
}

static void read_seed1(void *env, TestingT *t) {
    SgrEnv *e = env;
    Slice buf = sgr_buf(e), seed1 = sgr_seed1(e);
    cryptotest_set_global_random(t, 1);
    crypto_rand_read(buf, NULL);
    if (!bytes_equal(buf, seed1))
        testing_t_errorf_v(t, "rand.Read with seed 1 = %x; want %x", buf, seed1);

    crypto_rand_read(buf, NULL);
    if (bytes_equal(buf, seed1))
        testing_t_errorf_v(t, "rand.Read with seed 1 returned same output twice: %x",
                           buf);

    cryptotest_set_global_random(t, 1);
    crypto_rand_read(buf, NULL);
    if (!bytes_equal(buf, seed1))
        testing_t_errorf_v(t, "rand.Read with seed 1 after reset = %x; want %x", buf,
                           seed1);

    cryptotest_set_global_random(t, 1);
}

static void read_seed2(void *env, TestingT *t) {
    SgrEnv *e = env;
    Slice buf = sgr_buf(e), seed1 = sgr_seed1(e);
    cryptotest_set_global_random(t, 2);
    crypto_rand_read(buf, NULL);
    if (bytes_equal(buf, seed1))
        testing_t_errorf_v(t, "rand.Read with seed 2 = %x; want different from %x", buf,
                           seed1);
}

static void rand_read(void *env, TestingT *t) {
    SgrEnv *e = env;
    memset(e->buf, 0, sizeof e->buf);

    testing_t_run(t, BURROW_S("seed 1"), BURROW_FN(TestingTFunc, read_seed1, e));

    crypto_rand_read(sgr_buf(e), NULL);
    if (bytes_equal(sgr_buf(e), sgr_seed1(e)))
        testing_t_errorf_v(t, "rand.Read returned seeded output after test end");

    testing_t_run(t, BURROW_S("seed 2"), BURROW_FN(TestingTFunc, read_seed2, e));
}

static void reader_seed1(void *env, TestingT *t) {
    SgrEnv *e = env;
    Slice buf = sgr_buf(e), seed1 = sgr_seed1(e);
    cryptotest_set_global_random(t, 1);
    reader_read(buf);
    if (!bytes_equal(buf, seed1))
        testing_t_errorf_v(t, "rand.Reader.Read with seed 1 = %x; want %x", buf, seed1);

    cryptotest_set_global_random(t, 1);
}

static void reader_seed1_again(void *env, TestingT *t) {
    SgrEnv *e = env;
    Slice buf = sgr_buf(e), seed1 = sgr_seed1(e);
    cryptotest_set_global_random(t, 1);
    reader_read(buf);
    if (!bytes_equal(buf, seed1))
        testing_t_errorf_v(t, "rand.Reader.Read with seed 1 = %x; want %x", buf, seed1);
}

static void restore_reader(void *env) {
    SgrEnv *e = env;
    crypto_rand_reader = e->old_reader;
}

static void rand_reader(void *env, TestingT *t) {
    SgrEnv *e = env;
    memset(e->buf, 0, sizeof e->buf);

    testing_t_run(t, BURROW_S("seed 1"), BURROW_FN(TestingTFunc, reader_seed1, e));

    reader_read(sgr_buf(e));
    if (bytes_equal(sgr_buf(e), sgr_seed1(e)))
        testing_t_errorf_v(t, "rand.Reader.Read returned seeded output after test end");

    e->old_reader = crypto_rand_reader;
    testing_t_cleanup(t, BURROW_FN(Func, restore_reader, e));
    memset(e->fives, 5, sizeof e->fives);
    Slice want = slice_from(e->fives, 64, 64, TYPE_BYTE);
    bytes_reader_reset(&e->fives_reader, want);
    crypto_rand_reader = bytes_reader_as_io_reader(&e->fives_reader);

    testing_t_run(t, BURROW_S("seed 1 again"),
                  BURROW_FN(TestingTFunc, reader_seed1_again, e));

    reader_read(sgr_buf(e));
    if (!bytes_equal(sgr_buf(e), want))
        testing_t_errorf_v(t, "rand.Reader not restored");
}

/* A direct internal use of drbg.Read. */
static void mlkem_generate_key768_seed1(void *env, TestingT *t) {
    SgrEnv *e = env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    MlkemDecapsulationKey768 *exp =
        mlkem_new_decapsulation_key768(a, sgr_seed1(e), &err);
    if (BURROW_FAILED(err)) {
        arena_free(&ar);
        testing_t_fatalf_v(t, "mlkem.NewDecapsulationKey768: %v", err);
    }

    cryptotest_set_global_random(t, 1);
    MlkemDecapsulationKey768 *got = mlkem_generate_key768(a, &err);
    if (BURROW_FAILED(err)) {
        arena_free(&ar);
        testing_t_fatalf_v(t, "mlkem.GenerateKey768: %v", err);
    }

    Slice got_bytes = mlkem_decapsulation_key768_bytes(got, a);
    Slice exp_bytes = mlkem_decapsulation_key768_bytes(exp, a);
    if (!bytes_equal(got_bytes, exp_bytes))
        testing_t_errorf_v(t, "mlkem.GenerateKey768 with seed 1 = %x; want %x",
                           got_bytes, exp_bytes);
    arena_free(&ar);
}

static bool key_equal(const EcdsaPrivateKey *k, EcdsaPrivateKey *x) {
    Any v = BURROW_ANY(TYPE_ECDSA_PRIVATE_KEY, x);
    return ecdsa_private_key_equal(k, v);
}

/* An ignored passed-in Reader. */
static void ecdsa_generate_key_seed1(void *env, TestingT *t) {
    SgrEnv *e = env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    EcdsaPrivateKey *exp = ecdsa_parse_raw_private_key(
        a, elliptic_p384(), slice_sub(sgr_seed1(e), 0, 48), &err);
    if (BURROW_FAILED(err)) {
        arena_free(&ar);
        testing_t_fatalf_v(t, "ecdsa.ParseRawPrivateKey: %v", err);
    }

    cryptotest_set_global_random(t, 1);
    BytesReader br;
    bytes_reader_reset(&br, BURROW_B("this reader is ignored"));
    EcdsaPrivateKey *got =
        ecdsa_generate_key(a, elliptic_p384(), bytes_reader_as_io_reader(&br), &err);
    if (BURROW_FAILED(err)) {
        arena_free(&ar);
        testing_t_fatalf_v(t, "ecdsa.GenerateKey: %v", err);
    }

    if (!key_equal(got, exp)) {
        Slice got_d = big_int_bytes(got->d, a);
        Slice exp_d = big_int_bytes(exp->d, a);
        testing_t_errorf_v(t, "ecdsa.GenerateKey with seed 1 = %x; want %x", got_d,
                           exp_d);
    }
    arena_free(&ar);
}

/* Not in Go: t.Setenv("GODEBUG", ...) puts GODEBUG back when the test ends.
 * Here it is crypto/rand's copy of the setting, which forgets it. */
static void forget_godebug(void *env) {
    (void)env;
    burrow__crypto_rand_godebug_set(NULL);
}

/* Not in Go: the loop the subtest runs twice, which says with what in its
 * messages. Whether it saw both keys. */
static bool custom_rand_keys(TestingT *t, Alloc *a, Slice buf, EcdsaPrivateKey *exp1,
                             EcdsaPrivateKey *exp2, const char *with) {
    bool seen[2] = {false, false};
    for (int i = 0; i < 1000; i++) {
        BytesReader r;
        bytes_reader_reset(&r, buf);
        Error err = BURROW_NO_ERROR;
        EcdsaPrivateKey *got =
            ecdsa_generate_key(a, elliptic_p384(), bytes_reader_as_io_reader(&r), &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "ecdsa.GenerateKey: %v", err);
        if (key_equal(got, exp1)) {
            seen[0] = true;
        } else if (key_equal(got, exp2)) {
            seen[1] = true;
        } else {
            Slice got_d = big_int_bytes(got->d, a);
            Slice exp1_d = big_int_bytes(exp1->d, a);
            Slice exp2_d = big_int_bytes(exp2->d, a);
            Str w = str_from_cstr(with);
            testing_t_fatalf_v(
                t, "ecdsa.GenerateKey with custom reader%s = %x; want %x or %x", w,
                got_d, exp1_d, exp2_d);
        }
        if (seen[0] && seen[1])
            break;
    }
    return seen[0] && seen[1];
}

/* The passed-in Reader is used if cryptocustomrand=1 is set, and MaybeReadByte
 * is called on it. */
static void cryptocustomrand(void *env, TestingT *t) {
    (void)env;
    burrow__crypto_rand_godebug_set("cryptocustomrand=1");
    testing_t_cleanup(t, BURROW_FN(Func, forget_godebug, NULL));

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Byte b[49];
    memset(b, 0, sizeof b);
    b[0] = 42;
    for (int i = 2; i < 49; i++)
        b[i] = 1;
    Slice buf = slice_from(b, 49, 49, TYPE_BYTE);

    Error err = BURROW_NO_ERROR;
    EcdsaPrivateKey *exp1 =
        ecdsa_parse_raw_private_key(a, elliptic_p384(), slice_sub(buf, 0, 48), &err);
    if (BURROW_FAILED(err)) {
        arena_free(&ar);
        testing_t_fatalf_v(t, "ecdsa.ParseRawPrivateKey: %v", err);
    }
    EcdsaPrivateKey *exp2 =
        ecdsa_parse_raw_private_key(a, elliptic_p384(), slice_sub(buf, 1, 49), &err);
    if (BURROW_FAILED(err)) {
        arena_free(&ar);
        testing_t_fatalf_v(t, "ecdsa.ParseRawPrivateKey: %v", err);
    }

    if (!custom_rand_keys(t, a, buf, exp1, exp2, ""))
        testing_t_errorf_v(
            t,
            "ecdsa.GenerateKey with custom reader did not produce both expected keys");

    /* Again, with SetGlobalRandom. */
    cryptotest_set_global_random(t, 1);

    if (!custom_rand_keys(t, a, buf, exp1, exp2, " and SetGlobalRandom"))
        testing_t_errorf_v(t,
                           "ecdsa.GenerateKey with custom reader and SetGlobalRandom "
                           "did not produce both expected keys");
    arena_free(&ar);
}

static void TestSetGlobalRandom(TestingT *t) {
    SgrEnv e;
    memset(&e, 0, sizeof e);
    Error err = BURROW_NO_ERROR;
    Slice dst = slice_from(e.seed1, 64, 64, TYPE_BYTE);
    (void)hex_decode(dst, slice_from((Byte *)(uintptr_t)seed1_hex, 128, 128, TYPE_BYTE),
                     &err);

    testing_t_run(t, BURROW_S("rand.Read"), BURROW_FN(TestingTFunc, rand_read, &e));
    testing_t_run(t, BURROW_S("rand.Reader"), BURROW_FN(TestingTFunc, rand_reader, &e));
    testing_t_run(t, BURROW_S("mlkem.GenerateKey768"),
                  BURROW_FN(TestingTFunc, mlkem_generate_key768_seed1, &e));
    testing_t_run(t, BURROW_S("ecdsa.GenerateKey"),
                  BURROW_FN(TestingTFunc, ecdsa_generate_key_seed1, &e));
    testing_t_run(t, BURROW_S("cryptocustomrand=1"),
                  BURROW_FN(TestingTFunc, cryptocustomrand, NULL));
}

#define TESTS(X) X(TestSetGlobalRandom)

TESTING_MAIN(TESTS)
