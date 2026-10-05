/* Derived from golang.org/x/crypto's chacha20/chacha_test.go,
 * internal/poly1305/poly1305_test.go and
 * chacha20poly1305/chacha20poly1305_test.go, at the version Go vendors.
 * Go source: go1.27.1, golang.org/x/crypto v0.52.1-0.20260526024921-9beb694f9766.
 *
 * The vectors are in chacha20poly1305_test_gen.h, which
 * tools/gen-chacha20poly1305-tests.sh writes. burrow has one implementation of
 * each, the portable one, so TestSumGeneric and TestSum test the same code, as
 * they do in Go on a machine without assembly. ExampleNewX is a test here, as
 * the package is private. The tests after it are burrow's, for the errors, the
 * panics and sealing in place.
 *
 * Copyright 2016 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/crypto/cipher.h"
#include "burrow/crypto/rand.h"
#include "burrow/encoding/hex.h"
#include "burrow/math/rand.h"
#include "burrow/mem/heap.h"
#include "burrow/slices.h"

#include "../src/crypto/chacha20poly1305.h"

#include "chacha20poly1305_test_gen.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define LEN(x) (sizeof(x) / sizeof((x)[0]))

static volatile Byte sink;

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

/* A hex string the generator has cut into pieces, as C99 can only take so much
 * in one literal. */
static Slice unhex_parts(Alloc *a, const char *const parts[GEN_HEX_PARTS]) {
    Slice b[GEN_HEX_PARTS];
    int n = 0;
    while (n < GEN_HEX_PARTS && parts[n] != NULL) {
        b[n] = unhex(a, parts[n]);
        n++;
    }
    return n == 1 ? b[0] : slices_concat(a, b, n);
}

static Str hexs(Alloc *a, Slice b) {
    return hex_encode_to_string(a, b);
}

static Slice make_bytes(Alloc *a, Int n) {
    return slice_make(a, TYPE_BYTE, n, n);
}

static Byte *bp(Slice b) {
    return (Byte *)b.p;
}

/* unalignBytes: a copy of in that does not start on a 4 byte boundary. */
static Slice unalign_bytes(Alloc *a, Slice in) {
    Slice out = make_bytes(a, in.len + 1);
    if (((uintptr_t)out.p & 3) == 0)
        out = slice_sub(out, 1, in.len + 1);
    else
        out = slice_sub(out, 0, in.len);
    if (in.len > 0)
        memcpy(out.p, in.p, (size_t)in.len);
    return out;
}

/* Whether f panics, as the tests' panics helper finds out with recover. */
static bool panics(void (*f)(void *), void *env) {
    volatile bool got = false;
    BURROW_TRY {
        f(env);
    }
    BURROW_CATCH(r) {
        (void)r;
        got = true;
    }
    BURROW_TRY_END;
    return got;
}

/* ---------------------------------------------------------------- chacha20 */

static Chacha20Cipher *new_cipher(Alloc *a, Slice key, Slice nonce) {
    Error err = BURROW_NO_ERROR;
    Chacha20Cipher *s = chacha20_new_unauthenticated_cipher(a, key, nonce, &err);
    if (BURROW_FAILED(err))
        panic_str(error_text(err));
    return s;
}

static void TestNoOverlap(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < LEN(gen_chacha20_vectors); i++) {
        const GenChacha20Vector *c = &gen_chacha20_vectors[i];
        Chacha20Cipher *s = new_cipher(a, unhex(a, c->key), unhex(a, c->nonce));
        Slice input = unhex_parts(a, c->input);
        Slice output = make_bytes(a, input.len);
        chacha20_cipher_xor_key_stream(s, output, input);
        Slice want = unhex_parts(a, c->output);
        if (!bytes_equal(output, want))
            testing_t_errorf_v(t, "length=%d: got %s, want %s", (int)input.len,
                               hexs(a, output), hexs(a, want));
        arena_reset(&ar);
    }
    arena_free(&ar);
}

static void TestOverlap(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < LEN(gen_chacha20_vectors); i++) {
        const GenChacha20Vector *c = &gen_chacha20_vectors[i];
        Chacha20Cipher *s = new_cipher(a, unhex(a, c->key), unhex(a, c->nonce));
        Slice data = unhex_parts(a, c->input);
        chacha20_cipher_xor_key_stream(s, data, data);
        Slice want = unhex_parts(a, c->output);
        if (!bytes_equal(data, want))
            testing_t_errorf_v(t, "length=%d: got %s, want %s", (int)data.len,
                               hexs(a, data), hexs(a, want));
        arena_reset(&ar);
    }
    arena_free(&ar);
}

static void TestUnaligned(TestingT *t) {
    enum { MAX = 8 }; /* max offset (+1) to test */
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t k = 0; k < LEN(gen_chacha20_vectors); k++) {
        const GenChacha20Vector *c = &gen_chacha20_vectors[k];
        Slice data = unhex_parts(a, c->input);
        Slice want = unhex_parts(a, c->output);
        Slice input_buf = make_bytes(a, data.len + MAX);
        Slice output_buf = make_bytes(a, data.len + MAX);
        for (Int i = 0; i < MAX; i++) {     /* input offsets */
            for (Int j = 0; j < MAX; j++) { /* output offsets */
                Chacha20Cipher *s = new_cipher(a, unhex(a, c->key), unhex(a, c->nonce));
                Slice input = slice_sub(input_buf, i, i + data.len);
                Slice output = slice_sub(output_buf, j, j + data.len);
                slice_copy(input, data);
                chacha20_cipher_xor_key_stream(s, output, input);
                if (!bytes_equal(output, want))
                    testing_t_errorf_v(t, "length=%d: got %s, want %s", (int)data.len,
                                       hexs(a, output), hexs(a, want));
            }
        }
        arena_reset(&ar);
    }
    arena_free(&ar);
}

static void TestStep(TestingT *t) {
    static const Int steps[] = {1, 3, 4, 7, 8, 17, 24, 30, 64, 256};
    const Int nsteps = (Int)LEN(steps);
    Arena keep, ar;
    arena_init(&keep, NULL, 0);
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Alloc *ka = arena_allocator(&keep);
    MathRandRand *rnd = math_rand_new(ka, math_rand_new_source(ka, 123));
    for (size_t k = 0; k < LEN(gen_chacha20_vectors); k++) {
        const GenChacha20Vector *c = &gen_chacha20_vectors[k];
        Chacha20Cipher *s = new_cipher(a, unhex(a, c->key), unhex(a, c->nonce));
        Slice input = unhex_parts(a, c->input);
        Slice output = make_bytes(a, input.len);

        /* Step through the buffer, an amount at a time. */
        Int i = 0, step = steps[math_rand_rand_intn(rnd, nsteps)];
        while (i + step < input.len) {
            chacha20_cipher_xor_key_stream(s, slice_sub(output, i, i + step),
                                           slice_sub(input, i, i + step));
            if (i + step < input.len && bp(output)[i + step] != 0)
                testing_t_errorf_v(t, "length=%d, i=%d, step=%d: output overwritten",
                                   (int)input.len, (int)i, (int)step);
            i += step;
            step = steps[math_rand_rand_intn(rnd, nsteps)];
        }
        /* Finish the encryption. */
        chacha20_cipher_xor_key_stream(s, slice_sub(output, i, output.len),
                                       slice_sub(input, i, input.len));
        /* Ensure we tolerate a call with an empty input. */
        chacha20_cipher_xor_key_stream(s, slice_sub(output, output.len, output.len),
                                       slice_sub(input, input.len, input.len));

        Slice want = unhex_parts(a, c->output);
        if (!bytes_equal(output, want))
            testing_t_errorf_v(t, "length=%d: got %s, want %s", (int)input.len,
                               hexs(a, output), hexs(a, want));
        arena_reset(&ar);
    }
    arena_free(&ar);
    arena_free(&keep);
}

static Chacha20Cipher *zero_cipher(Alloc *a) {
    Byte key[CHACHA20_KEY_SIZE] = {0};
    Byte nonce[CHACHA20_NONCE_SIZE] = {0};
    return new_cipher(a, bs(key, sizeof key), bs(nonce, sizeof nonce));
}

typedef struct {
    Chacha20Cipher *s;
    uint32_t counter;
    Slice dst, src;
} CipherCall;

static void call_set_counter(void *env) {
    CipherCall *c = env;
    chacha20_cipher_set_counter(c->s, c->counter);
}

static void call_xor(void *env) {
    CipherCall *c = env;
    chacha20_cipher_xor_key_stream(c->s, c->dst, c->src);
}

static void TestSetCounter(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    /* Two 64 byte blocks. */
    Byte src[128];
    for (int i = 0; i < 128; i += 4)
        memcpy(src + i, "test", 4);
    Byte dst1[128] = {0}, dst2[128] = {0};

    Chacha20Cipher *s = zero_cipher(a);
    chacha20_cipher_xor_key_stream(s, bs(dst1, 128), bs(src, 128));
    s = zero_cipher(a);
    chacha20_cipher_set_counter(s, 1);
    chacha20_cipher_xor_key_stream(s, bs(dst2 + 64, 64), bs(src + 64, 64));
    if (memcmp(dst1 + 64, dst2 + 64, 64) != 0)
        testing_t_error_v(t, "failed to produce identical output using SetCounter");

    /* Make sure the buffer is reset by SetCounter. */
    s = zero_cipher(a);
    chacha20_cipher_xor_key_stream(s, bs(dst1, 70), bs(src, 70));
    s = zero_cipher(a);
    Byte zero[1] = {0};
    chacha20_cipher_xor_key_stream(s, bs(zero, 1), bs(zero, 1));
    chacha20_cipher_set_counter(s, 1);
    chacha20_cipher_xor_key_stream(s, bs(dst2 + 64, 6), bs(src + 64, 6));
    if (memcmp(dst1 + 64, dst2 + 64, 6) != 0)
        testing_t_error_v(t, "SetCounter did not reset buffer");

    /* Check that SetCounter panics when used to go backwards. */
    CipherCall c = {s, 0, {0}, {0}};
    if (!panics(call_set_counter, &c))
        testing_t_error_v(t, "counter decreasing should trigger a panic");
    arena_free(&ar);
}

static void check_last_block(TestingT *t, Alloc *a, Slice b) {
    static const char last_block[] =
        "ace4cd09e294d1912d4ad205d06f95d9c2f2bfcf453e8753f128765b62215f4d"
        "92c74f2f626c6a640c0b1284d839ec81f1696281dafc3e684593937023b58b1d";
    Str got = hexs(a, b);
    if (!str_eq(got, str_from_cstr(last_block)))
        testing_t_errorf_v(t, "wrong output for the last block, got %s, want %s", got,
                           str_from_cstr(last_block));
}

static void TestLastBlock(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    /* setting the counter to 0xffffffff and crypting multiple blocks should
     * trigger a panic */
    Byte blocks[CHACHA20_BLOCK_SIZE * 2] = {0};
    Chacha20Cipher *s = zero_cipher(a);
    chacha20_cipher_set_counter(s, 0xffffffff);
    CipherCall c = {s, 0, bs(blocks, sizeof blocks), bs(blocks, sizeof blocks)};
    if (!panics(call_xor, &c))
        testing_t_error_v(t, "crypting multiple blocks should trigger a panic");

    /* setting the counter to 0xffffffff - 1 and crypting two blocks should
     * work */
    s = zero_cipher(a);
    chacha20_cipher_set_counter(s, 0xffffffff - 1);
    c = (CipherCall){s, 0, bs(blocks, sizeof blocks), bs(blocks, sizeof blocks)};
    if (panics(call_xor, &c))
        testing_t_error_v(t, "crypting the last blocks should not trigger a panic");
    check_last_block(t, a, bs(blocks + CHACHA20_BLOCK_SIZE, CHACHA20_BLOCK_SIZE));
    /* once all the keystream is spent, setting the counter should panic */
    c = (CipherCall){s, 0xffffffff, {0}, {0}};
    if (!panics(call_set_counter, &c))
        testing_t_error_v(t,
                          "setting the counter after overflow should trigger a panic");
    /* crypting a subsequent block *should* panic */
    Byte block[CHACHA20_BLOCK_SIZE] = {0};
    c = (CipherCall){s, 0, bs(block, sizeof block), bs(block, sizeof block)};
    if (!panics(call_xor, &c))
        testing_t_error_v(t, "crypting after overflow should trigger a panic");

    /* if we crypt less than a full block, we should be able to crypt the rest
     * in a subsequent call without panicking */
    s = zero_cipher(a);
    chacha20_cipher_set_counter(s, 0xffffffff);
    c = (CipherCall){s, 0, bs(block, 7), bs(block, 7)};
    if (panics(call_xor, &c))
        testing_t_error_v(t,
                          "crypting part of the last block should not trigger a panic");
    c = (CipherCall){s, 0, bs(block + 7, CHACHA20_BLOCK_SIZE - 7),
                     bs(block + 7, CHACHA20_BLOCK_SIZE - 7)};
    if (panics(call_xor, &c))
        testing_t_error_v(t,
                          "crypting part of the last block should not trigger a panic");
    check_last_block(t, a, bs(block, sizeof block));
    /* as before, a third call should trigger a panic because all keystream is
     * spent */
    c = (CipherCall){s, 0, bs(block, 1), bs(block, 1)};
    if (!panics(call_xor, &c))
        testing_t_error_v(t, "crypting after overflow should trigger a panic");
    arena_free(&ar);
}

static void TestHChaCha20(TestingT *t) {
    /* See draft-irtf-cfrg-xchacha-00, Section 2.2.1. */
    Byte key[32];
    for (int i = 0; i < 32; i++)
        key[i] = (Byte)i;
    Byte nonce[16] = {0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0x4a,
                      0x00, 0x00, 0x00, 0x00, 0x31, 0x41, 0x59, 0x27};
    Byte expected[32] = {0x82, 0x41, 0x3b, 0x42, 0x27, 0xb2, 0x7b, 0xfe,
                         0xd3, 0x0e, 0x42, 0x50, 0x8a, 0x87, 0x7d, 0x73,
                         0xa0, 0xf9, 0xe4, 0xd5, 0x8a, 0x74, 0xa8, 0x53,
                         0xc1, 0x2e, 0xc4, 0x13, 0x26, 0xd3, 0xec, 0xdc};
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    Slice result = chacha20_hchacha20(a, bs(key, 32), bs(nonce, 16), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    if (!bytes_equal(bs(expected, 32), result))
        testing_t_errorf_v(t, "want %s, got %s", hexs(a, bs(expected, 32)),
                           hexs(a, result));
    arena_free(&ar);
}

typedef struct {
    Int step, count;
} ChachaBench;

static void benchmark_chacha20(void *env, TestingB *b) {
    const ChachaBench *p = env;
    Int tot = p->step * p->count;
    Slice src = make_bytes(heap_allocator(), tot);
    Slice dst = make_bytes(heap_allocator(), tot);
    Byte key[CHACHA20_KEY_SIZE] = {0};
    Byte nonce[CHACHA20_NONCE_SIZE] = {0};
    testing_b_set_bytes(b, (int64_t)tot);
    testing_b_reset_timer(b);
    for (Int i = 0; i < testing_b_n(b); i++) {
        Chacha20Cipher c;
        Error err = BURROW_NO_ERROR;
        Chacha20Cipher *h = chacha20_new_unauthenticated_cipher(
            heap_allocator(), bs(key, sizeof key), bs(nonce, sizeof nonce), &err);
        c = *h;
        mem_free(heap_allocator(), h, sizeof *h, _Alignof(Chacha20Cipher));
        for (Int j = 0; j < tot; j += p->step)
            chacha20_cipher_xor_key_stream(&c, slice_sub(dst, j, tot),
                                           slice_sub(src, j, j + p->step));
    }
    sink ^= bp(dst)[0];
    mem_free(heap_allocator(), src.p, (size_t)src.cap, 1);
    mem_free(heap_allocator(), dst.p, (size_t)dst.cap, 1);
}

static void BenchmarkChaCha20(TestingB *b) {
    static const struct {
        const char *name;
        ChachaBench p;
    } cases[] = {
        {"64", {64, 1}},         {"256", {256, 1}},     {"10x25", {10, 25}},
        {"4096", {4096, 1}},     {"100x40", {100, 40}}, {"65536", {65536, 1}},
        {"1000x65", {1000, 65}},
    };
    for (size_t i = 0; i < LEN(cases); i++)
        testing_b_run(b, str_from_cstr(cases[i].name),
                      BURROW_FN(TestingBFunc, benchmark_chacha20,
                                (void *)(uintptr_t)&cases[i].p));
}

/* ---------------------------------------------------------------- poly1305 */

static void vector_key(const GenPoly1305Vector *v, Byte key[32]) {
    Byte buf[32];
    Error err = BURROW_NO_ERROR;
    Int n = hex_decode(bs(buf, 32),
                       slice_from((void *)(uintptr_t)v->key, (Int)strlen(v->key),
                                  (Int)strlen(v->key), TYPE_BYTE),
                       &err);
    if (BURROW_FAILED(err) || n != 32)
        panic_str(BURROW_S("invalid key"));
    memcpy(key, buf, 32);
}

static void vector_tag(const GenPoly1305Vector *v, Byte tag[16]) {
    Byte buf[16];
    Error err = BURROW_NO_ERROR;
    Int n = hex_decode(bs(buf, 16),
                       slice_from((void *)(uintptr_t)v->tag, (Int)strlen(v->tag),
                                  (Int)strlen(v->tag), TYPE_BYTE),
                       &err);
    if (BURROW_FAILED(err) || n != 16)
        panic_str(BURROW_S("invalid tag"));
    memcpy(tag, buf, 16);
}

static void test_sum(TestingT *t, bool unaligned) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Byte tag[16];
    for (size_t i = 0; i < LEN(gen_poly1305_vectors); i++) {
        const GenPoly1305Vector *v = &gen_poly1305_vectors[i];
        if (v->has_state)
            continue;

        Slice in = unhex_parts(a, v->in);
        if (unaligned)
            in = unalign_bytes(a, in);
        Byte key[32], want[16];
        vector_key(v, key);
        vector_tag(v, want);
        poly1305_sum(tag, in, key);
        if (memcmp(tag, want, 16) != 0)
            testing_t_errorf_v(t, "%d: expected %s, got %s", (int)i,
                               hexs(a, bs(want, 16)), hexs(a, bs(tag, 16)));
        if (!poly1305_verify(tag, in, key))
            testing_t_errorf_v(t, "%d: tag didn't verify", (int)i);
        /* If the key is zero, the tag will always be zero, independent of the
         * input. */
        static const Byte zero_key[32] = {0};
        if (in.len > 0 && memcmp(key, zero_key, 32) != 0) {
            bp(in)[0] ^= 0xff;
            if (poly1305_verify(tag, in, key))
                testing_t_errorf_v(t, "%d: tag verified after altering the input",
                                   (int)i);
            bp(in)[0] ^= 0xff;
        }
        /* If the input is empty, the tag only depends on the second half of the
         * key. */
        if (in.len > 0) {
            key[0] ^= 0xff;
            if (poly1305_verify(tag, in, key))
                testing_t_errorf_v(t, "%d: tag verified after altering the key",
                                   (int)i);
            key[0] ^= 0xff;
        }
        tag[0] ^= 0xff;
        if (poly1305_verify(tag, in, key))
            testing_t_errorf_v(t, "%d: tag verified after altering the tag", (int)i);
        tag[0] ^= 0xff;
        arena_reset(&ar);
    }
    arena_free(&ar);
}

static void TestBurnin(TestingT *t) {
    /* This test can take minutes to run, as it does 1e10 iterations. */
    testing_t_skip_v(t, "skipping without -stress");
}

static void TestSum(TestingT *t) {
    test_sum(t, false);
}

static void TestSumUnaligned(TestingT *t) {
    test_sum(t, true);
}

static void TestSumGeneric(TestingT *t) {
    test_sum(t, false);
}

static void TestSumGenericUnaligned(TestingT *t) {
    test_sum(t, true);
}

/* testWrite and testWriteGeneric, which differ in Go only in which MAC they
 * make. check_verify is the part of testWrite the generic MAC has no method
 * for. */
static void test_write(TestingT *t, bool unaligned, bool check_verify) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < LEN(gen_poly1305_vectors); i++) {
        const GenPoly1305Vector *v = &gen_poly1305_vectors[i];
        Byte key[32], tag[16];
        vector_key(v, key);
        vector_tag(v, tag);
        Slice input = unhex_parts(a, v->in);
        if (unaligned)
            input = unalign_bytes(a, input);

        Poly1305Mac h;
        poly1305_mac_init(&h, key);
        if (v->has_state)
            memcpy(h.h, v->state, sizeof h.h);
        Int third = input.len / 3;
        Int n = poly1305_mac_write(&h, slice_sub(input, 0, third));
        if (n != third)
            testing_t_errorf_v(t, "#%d: unexpected Write results: n = %d", (int)i,
                               (int)n);
        n = poly1305_mac_write(&h, slice_sub(input, third, input.len));
        if (n != input.len - third)
            testing_t_errorf_v(t, "#%d: unexpected Write results: n = %d", (int)i,
                               (int)n);
        Byte out[16];
        Slice got = poly1305_mac_sum(&h, a, slice_from(out, 0, 16, TYPE_BYTE));
        if (got.p != out || got.len != 16 || memcmp(out, tag, 16) != 0)
            testing_t_errorf_v(t, "%d: expected %s, got %s", (int)i,
                               hexs(a, bs(tag, 16)), hexs(a, got));
        if (check_verify) {
            if (!poly1305_mac_verify(&h, bs(tag, 16)))
                testing_t_errorf_v(t, "%d: Verify failed", (int)i);
            tag[0] ^= 0xff;
            if (poly1305_mac_verify(&h, bs(tag, 16)))
                testing_t_errorf_v(t, "%d: Verify succeeded after modifying the tag",
                                   (int)i);
        }
        arena_reset(&ar);
    }
    arena_free(&ar);
}

static void TestWriteGeneric(TestingT *t) {
    test_write(t, false, false);
}

static void TestWriteGenericUnaligned(TestingT *t) {
    test_write(t, true, false);
}

static void TestWrite(TestingT *t) {
    test_write(t, false, true);
}

static void TestWriteUnaligned(TestingT *t) {
    test_write(t, true, true);
}

static void call_write_after_sum(void *env) {
    Poly1305Mac *h = env;
    Byte b[1] = {0};
    poly1305_mac_write(h, bs(b, 1));
}

/* burrow's: MAC.Write after Sum or Verify panics, as Go's does. */
static void TestWriteAfterSum(TestingT *t) {
    Byte key[32] = {1};
    Poly1305Mac h;
    poly1305_mac_init(&h, key);
    Byte out[16];
    poly1305_mac_sum(&h, heap_allocator(), slice_from(out, 0, 16, TYPE_BYTE));
    if (!panics(call_write_after_sum, &h))
        testing_t_error_v(t, "Write after Sum did not panic");
    poly1305_mac_init(&h, key);
    poly1305_mac_verify(&h, bs(out, 16));
    if (!panics(call_write_after_sum, &h))
        testing_t_error_v(t, "Write after Verify did not panic");
    if (poly1305_mac_size(&h) != POLY1305_TAG_SIZE)
        testing_t_error_v(t, "Size is not TagSize");
    Poly1305Mac *hp = poly1305_new(heap_allocator(), key);
    if (hp == NULL || hp->r[0] != h.r[0] || hp->s[0] != h.s[0])
        testing_t_error_v(t, "New does not match the MAC made in place");
    if (hp != NULL)
        mem_free(heap_allocator(), hp, sizeof *hp, _Alignof(Poly1305Mac));
}

typedef struct {
    Int size;
    bool unaligned;
} PolyBench;

static void benchmark_sum(void *env, TestingB *b) {
    const PolyBench *p = env;
    Byte out[16];
    Byte key[32] = {0};
    Slice buf = make_bytes(heap_allocator(), p->size + 1);
    Slice in = slice_sub(buf, 0, p->size);
    if (p->unaligned)
        in = ((uintptr_t)buf.p & 3) == 0 ? slice_sub(buf, 1, p->size + 1) : in;
    crypto_rand_read(in, NULL);
    testing_b_set_bytes(b, (int64_t)in.len);
    testing_b_reset_timer(b);
    for (Int i = 0; i < testing_b_n(b); i++)
        poly1305_sum(out, in, key);
    sink ^= out[0];
    mem_free(heap_allocator(), buf.p, (size_t)buf.cap, 1);
}

static void benchmark_write(void *env, TestingB *b) {
    const PolyBench *p = env;
    Byte key[32] = {0};
    Poly1305Mac h;
    poly1305_mac_init(&h, key);
    Slice buf = make_bytes(heap_allocator(), p->size + 1);
    Slice in = slice_sub(buf, 0, p->size);
    if (p->unaligned)
        in = ((uintptr_t)buf.p & 3) == 0 ? slice_sub(buf, 1, p->size + 1) : in;
    crypto_rand_read(in, NULL);
    testing_b_set_bytes(b, (int64_t)in.len);
    testing_b_reset_timer(b);
    for (Int i = 0; i < testing_b_n(b); i++)
        poly1305_mac_write(&h, in);
    sink ^= (Byte)h.h[0];
    mem_free(heap_allocator(), buf.p, (size_t)buf.cap, 1);
}

static const PolyBench poly_bench_64 = {64, false};
static const PolyBench poly_bench_1k = {1024, false};
static const PolyBench poly_bench_2m = {2 * 1024 * 1024, false};
static const PolyBench poly_bench_64u = {64, true};
static const PolyBench poly_bench_1ku = {1024, true};
static const PolyBench poly_bench_2mu = {2 * 1024 * 1024, true};

static void Benchmark64(TestingB *b) {
    benchmark_sum((void *)(uintptr_t)&poly_bench_64, b);
}
static void Benchmark1K(TestingB *b) {
    benchmark_sum((void *)(uintptr_t)&poly_bench_1k, b);
}
static void Benchmark2M(TestingB *b) {
    benchmark_sum((void *)(uintptr_t)&poly_bench_2m, b);
}
static void Benchmark64Unaligned(TestingB *b) {
    benchmark_sum((void *)(uintptr_t)&poly_bench_64u, b);
}
static void Benchmark1KUnaligned(TestingB *b) {
    benchmark_sum((void *)(uintptr_t)&poly_bench_1ku, b);
}
static void Benchmark2MUnaligned(TestingB *b) {
    benchmark_sum((void *)(uintptr_t)&poly_bench_2mu, b);
}
static void BenchmarkWrite64(TestingB *b) {
    benchmark_write((void *)(uintptr_t)&poly_bench_64, b);
}
static void BenchmarkWrite1K(TestingB *b) {
    benchmark_write((void *)(uintptr_t)&poly_bench_1k, b);
}
static void BenchmarkWrite2M(TestingB *b) {
    benchmark_write((void *)(uintptr_t)&poly_bench_2m, b);
}
static void BenchmarkWrite64Unaligned(TestingB *b) {
    benchmark_write((void *)(uintptr_t)&poly_bench_64u, b);
}
static void BenchmarkWrite1KUnaligned(TestingB *b) {
    benchmark_write((void *)(uintptr_t)&poly_bench_1ku, b);
}
static void BenchmarkWrite2MUnaligned(TestingB *b) {
    benchmark_write((void *)(uintptr_t)&poly_bench_2mu, b);
}

/* -------------------------------------------------------- chacha20poly1305 */

static CipherAEAD new_aead(Alloc *a, Slice key, Int nonce_size, Error *err) {
    if (nonce_size == CHACHA20POLY1305_NONCE_SIZE)
        return chacha20poly1305_new(a, key, err);
    return chacha20poly1305_new_x(a, key, err);
}

static bool opens(CipherAEAD aead, Alloc *a, Slice nonce, Slice ct, Slice ad) {
    Error err = BURROW_NO_ERROR;
    cipher_aead_open(aead, a, slice_nil(TYPE_BYTE), nonce, ct, ad, &err);
    return BURROW_OK(err);
}

static void TestVectors(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < LEN(gen_chacha20poly1305_vectors); i++) {
        const GenChacha20Poly1305Vector *test = &gen_chacha20poly1305_vectors[i];
        Slice key = unhex(a, test->key);
        Slice nonce = unhex(a, test->nonce);
        Slice ad = unhex_parts(a, test->aad);
        Slice plaintext = unhex_parts(a, test->plaintext);

        if (nonce.len != CHACHA20POLY1305_NONCE_SIZE &&
            nonce.len != CHACHA20POLY1305_NONCE_SIZE_X)
            testing_t_fatalf_v(t, "#%d: wrong nonce length: %d", (int)i,
                               (int)nonce.len);
        Error err = BURROW_NO_ERROR;
        CipherAEAD aead = new_aead(a, key, nonce.len, &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "%s", error_text(err));

        Slice ct =
            cipher_aead_seal(aead, a, slice_nil(TYPE_BYTE), nonce, plaintext, ad);
        Slice want = unhex_parts(a, test->out);
        if (!bytes_equal(ct, want)) {
            testing_t_errorf_v(t, "#%d: got %s, want %s", (int)i, hexs(a, ct),
                               hexs(a, want));
            arena_reset(&ar);
            continue;
        }

        Slice plaintext2 =
            cipher_aead_open(aead, a, slice_nil(TYPE_BYTE), nonce, ct, ad, &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "#%d: Open failed", (int)i);
            arena_reset(&ar);
            continue;
        }

        if (!bytes_equal(plaintext, plaintext2)) {
            testing_t_errorf_v(t, "#%d: plaintext's don't match: got %s vs %s", (int)i,
                               hexs(a, plaintext2), hexs(a, plaintext));
            arena_reset(&ar);
            continue;
        }

        if (ad.len > 0) {
            Int alter_ad_idx = math_rand_intn(ad.len);
            bp(ad)[alter_ad_idx] ^= 0x80;
            if (opens(aead, a, nonce, ct, ad))
                testing_t_errorf_v(
                    t, "#%d: Open was successful after altering additional data",
                    (int)i);
            bp(ad)[alter_ad_idx] ^= 0x80;
        }

        Int alter_nonce_idx = math_rand_intn(cipher_aead_nonce_size(aead));
        bp(nonce)[alter_nonce_idx] ^= 0x80;
        if (opens(aead, a, nonce, ct, ad))
            testing_t_errorf_v(t, "#%d: Open was successful after altering nonce",
                               (int)i);
        bp(nonce)[alter_nonce_idx] ^= 0x80;

        Int alter_ct_idx = math_rand_intn(ct.len);
        bp(ct)[alter_ct_idx] ^= 0x80;
        if (opens(aead, a, nonce, ct, ad))
            testing_t_errorf_v(t, "#%d: Open was successful after altering ciphertext",
                               (int)i);
        bp(ct)[alter_ct_idx] ^= 0x80;
        arena_reset(&ar);
    }
    arena_free(&ar);
}

static void test_random(void *env, TestingT *t) {
    Int nonce_size = *(const Int *)env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (int i = 0; i < 256; i++) {
        Slice nonce = make_bytes(a, nonce_size);
        Byte key[32];

        Int al = math_rand_intn(128);
        Int pl = math_rand_intn(16384);
        Slice ad = make_bytes(a, al);
        Slice plaintext = make_bytes(a, pl);
        crypto_rand_read(bs(key, 32), NULL);
        crypto_rand_read(nonce, NULL);
        crypto_rand_read(ad, NULL);
        crypto_rand_read(plaintext, NULL);

        Error err = BURROW_NO_ERROR;
        CipherAEAD aead = new_aead(a, bs(key, 32), nonce_size, &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "%s", error_text(err));

        Slice ct =
            cipher_aead_seal(aead, a, slice_nil(TYPE_BYTE), nonce, plaintext, ad);

        Slice plaintext2 =
            cipher_aead_open(aead, a, slice_nil(TYPE_BYTE), nonce, ct, ad, &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "Random #%d: Open failed", i);
            arena_reset(&ar);
            continue;
        }

        if (!bytes_equal(plaintext, plaintext2)) {
            testing_t_errorf_v(t, "Random #%d: plaintext's don't match: got %s vs %s",
                               i, hexs(a, plaintext2), hexs(a, plaintext));
            arena_reset(&ar);
            continue;
        }

        if (ad.len > 0) {
            Int alter_ad_idx = math_rand_intn(ad.len);
            bp(ad)[alter_ad_idx] ^= 0x80;
            if (opens(aead, a, nonce, ct, ad))
                testing_t_errorf_v(
                    t, "Random #%d: Open was successful after altering additional data",
                    i);
            bp(ad)[alter_ad_idx] ^= 0x80;
        }

        Int alter_nonce_idx = math_rand_intn(cipher_aead_nonce_size(aead));
        bp(nonce)[alter_nonce_idx] ^= 0x80;
        if (opens(aead, a, nonce, ct, ad))
            testing_t_errorf_v(
                t, "Random #%d: Open was successful after altering nonce", i);
        bp(nonce)[alter_nonce_idx] ^= 0x80;

        Int alter_ct_idx = math_rand_intn(ct.len);
        bp(ct)[alter_ct_idx] ^= 0x80;
        if (opens(aead, a, nonce, ct, ad))
            testing_t_errorf_v(
                t, "Random #%d: Open was successful after altering ciphertext", i);
        bp(ct)[alter_ct_idx] ^= 0x80;
        arena_reset(&ar);
    }
    arena_free(&ar);
}

static void TestRandom(TestingT *t) {
    static const Int standard = CHACHA20POLY1305_NONCE_SIZE;
    static const Int x = CHACHA20POLY1305_NONCE_SIZE_X;
    testing_t_run(t, BURROW_S("Standard"),
                  BURROW_FN(TestingTFunc, test_random, (void *)(uintptr_t)&standard));
    testing_t_run(t, BURROW_S("X"),
                  BURROW_FN(TestingTFunc, test_random, (void *)(uintptr_t)&x));
}

/* ExampleNewX, as a test, since the package is private. */
static void TestExampleNewX(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Byte key[CHACHA20POLY1305_KEY_SIZE];
    crypto_rand_read(bs(key, sizeof key), NULL);

    Error err = BURROW_NO_ERROR;
    CipherAEAD aead = chacha20poly1305_new_x(a, bs(key, sizeof key), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));

    /* Encryption. */
    Slice msg = slice_from((void *)(uintptr_t)"Gophers, gophers, gophers everywhere!",
                           37, 37, TYPE_BYTE);
    /* Select a random nonce, and leave capacity for the ciphertext. */
    Int ns = cipher_aead_nonce_size(aead);
    Slice nonce =
        slice_make(a, TYPE_BYTE, ns, ns + msg.len + cipher_aead_overhead(aead));
    crypto_rand_read(nonce, NULL);
    /* Encrypt the message and append the ciphertext to the nonce. */
    Slice encrypted_msg =
        cipher_aead_seal(aead, a, nonce, nonce, msg, slice_nil(TYPE_BYTE));
    if (encrypted_msg.p != nonce.p)
        testing_t_error_v(t, "Seal did not append in place");

    /* Decryption. */
    if (encrypted_msg.len < ns)
        testing_t_fatal_v(t, "ciphertext too short");
    /* Split nonce and ciphertext. */
    Slice n2 = slice_sub(encrypted_msg, 0, ns);
    Slice ciphertext = slice_sub(encrypted_msg, ns, encrypted_msg.len);
    /* Decrypt the message and check it wasn't tampered with. */
    Slice plaintext = cipher_aead_open(aead, a, slice_nil(TYPE_BYTE), n2, ciphertext,
                                       slice_nil(TYPE_BYTE), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    if (!bytes_equal(plaintext, msg))
        testing_t_errorf_v(t, "got %s", str_from_bytes(plaintext.p, plaintext.len));
    arena_free(&ar);
}

typedef struct {
    CipherAEAD aead;
    Slice nonce, text, ad;
    bool open;
} AeadCall;

static void call_aead(void *env) {
    AeadCall *c = env;
    Error err = BURROW_NO_ERROR;
    if (c->open)
        cipher_aead_open(c->aead, heap_allocator(), slice_nil(TYPE_BYTE), c->nonce,
                         c->text, c->ad, &err);
    else
        cipher_aead_seal(c->aead, heap_allocator(), slice_nil(TYPE_BYTE), c->nonce,
                         c->text, c->ad);
}

/* burrow's: the errors of the constructors and of a short ciphertext, and the
 * panics for a nonce of the wrong size, with Go's messages. */
static void TestErrors(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Byte key[33] = {0};
    Byte nonce[24] = {0};
    Error err = BURROW_NO_ERROR;

    static const struct {
        const char *want;
        int which;
        Int key, nonce;
    } cases[] = {
        {"chacha20poly1305: bad key length", 0, 31, 0},
        {"chacha20poly1305: bad key length", 1, 33, 0},
        {"chacha20: wrong key size", 2, 16, 12},
        {"chacha20: wrong nonce size", 2, 32, 8},
        {"chacha20: wrong HChaCha20 key size", 3, 31, 16},
        {"chacha20: wrong HChaCha20 nonce size", 3, 32, 24},
    };
    for (size_t i = 0; i < LEN(cases); i++) {
        err = BURROW_NO_ERROR;
        Slice k = bs(key, cases[i].key), n = bs(nonce, cases[i].nonce);
        switch (cases[i].which) {
        case 0:
            chacha20poly1305_new(a, k, &err);
            break;
        case 1:
            chacha20poly1305_new_x(a, k, &err);
            break;
        case 2:
            chacha20_new_unauthenticated_cipher(a, k, n, &err);
            break;
        default:
            chacha20_hchacha20(a, k, n, &err);
            break;
        }
        if (BURROW_OK(err) || !str_eq(error_text(err), str_from_cstr(cases[i].want)))
            testing_t_errorf_v(t, "#%d: got %s, want %s", (int)i,
                               BURROW_OK(err) ? BURROW_S("no error") : error_text(err),
                               str_from_cstr(cases[i].want));
    }

    for (int x = 0; x < 2; x++) {
        Int ns = x ? CHACHA20POLY1305_NONCE_SIZE_X : CHACHA20POLY1305_NONCE_SIZE;
        CipherAEAD aead = new_aead(a, bs(key, 32), ns, &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "%s", error_text(err));
        if (cipher_aead_nonce_size(aead) != ns ||
            cipher_aead_overhead(aead) != CHACHA20POLY1305_OVERHEAD)
            testing_t_errorf_v(t, "x=%d: NonceSize %d, Overhead %d", x,
                               (int)cipher_aead_nonce_size(aead),
                               (int)cipher_aead_overhead(aead));

        Byte ct[15] = {0};
        cipher_aead_open(aead, a, slice_nil(TYPE_BYTE), bs(nonce, ns), bs(ct, 15),
                         slice_nil(TYPE_BYTE), &err);
        if (!errors_is(err, burrow__chacha20poly1305_err_open))
            testing_t_errorf_v(t, "x=%d: a short ciphertext opened", x);

        AeadCall c = {aead, bs(nonce, ns - 1), bs(ct, 15), slice_nil(TYPE_BYTE), false};
        if (!panics(call_aead, &c))
            testing_t_errorf_v(t, "x=%d: Seal took a short nonce", x);
        c.open = true;
        if (!panics(call_aead, &c))
            testing_t_errorf_v(t, "x=%d: Open took a short nonce", x);
    }
    arena_free(&ar);
}

/* burrow's: Seal into plaintext[:0] and Open into ciphertext[:0], which the
 * AEAD contract allows, and an Open that fails clears what it wrote. */
static void TestInPlace(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < LEN(gen_chacha20poly1305_vectors); i++) {
        const GenChacha20Poly1305Vector *test = &gen_chacha20poly1305_vectors[i];
        Slice key = unhex(a, test->key);
        Slice nonce = unhex(a, test->nonce);
        Slice ad = unhex_parts(a, test->aad);
        Slice plaintext = unhex_parts(a, test->plaintext);
        Slice want = unhex_parts(a, test->out);
        Error err = BURROW_NO_ERROR;
        CipherAEAD aead = new_aead(a, key, nonce.len, &err);

        Slice buf = slice_make(a, TYPE_BYTE, plaintext.len, plaintext.len + 16);
        slice_copy(buf, plaintext);
        Slice ct = cipher_aead_seal(aead, a, slice_sub(buf, 0, 0), nonce, buf, ad);
        if (ct.p != buf.p || !bytes_equal(ct, want)) {
            testing_t_errorf_v(t, "#%d: sealing in place gave %s", (int)i, hexs(a, ct));
            arena_reset(&ar);
            continue;
        }
        Slice pt = cipher_aead_open(aead, a, slice_sub(ct, 0, 0), nonce, ct, ad, &err);
        if (BURROW_FAILED(err) || pt.p != ct.p || !bytes_equal(pt, plaintext))
            testing_t_errorf_v(t, "#%d: opening in place failed", (int)i);

        if (plaintext.len > 0) {
            Slice ct2 =
                cipher_aead_seal(aead, a, slice_nil(TYPE_BYTE), nonce, plaintext, ad);
            Slice out = make_bytes(a, plaintext.len);
            memset(out.p, 0xaa, (size_t)out.len);
            bp(ct2)[ct2.len - 1] ^= 1;
            Slice r =
                cipher_aead_open(aead, a, slice_sub(out, 0, 0), nonce, ct2, ad, &err);
            bool cleared = true;
            for (Int j = 0; j < out.len; j++)
                cleared = cleared && bp(out)[j] == 0;
            if (BURROW_OK(err) || r.p != NULL || !cleared)
                testing_t_errorf_v(t, "#%d: a failed Open left the output behind",
                                   (int)i);
        }
        arena_reset(&ar);
    }
    arena_free(&ar);
}

/* burrow's: the Cipher as a cipher.Stream gives the same key stream. */
static void TestCipherStream(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    const GenChacha20Vector *c = &gen_chacha20_vectors[1];
    Chacha20Cipher *s = new_cipher(a, unhex(a, c->key), unhex(a, c->nonce));
    CipherStream st = chacha20_cipher_as_cipher_stream(s);
    Slice data = unhex_parts(a, c->input);
    cipher_stream_xor_key_stream(st, data, data);
    if (!bytes_equal(data, unhex_parts(a, c->output)))
        testing_t_errorf_v(t, "got %s", hexs(a, data));
    arena_free(&ar);
}

typedef struct {
    Int length, nonce_size;
    bool open;
} AeadBench;

static void benchmark_aead(void *env, TestingB *b) {
    const AeadBench *p = env;
    Alloc *h = heap_allocator();
    testing_b_report_allocs(b);
    testing_b_set_bytes(b, (int64_t)p->length);

    Byte key[32] = {0};
    Byte nonce[24] = {0};
    Byte ad[13] = {0};
    Slice buf = make_bytes(h, p->length);
    Error err = BURROW_NO_ERROR;
    CipherAEAD aead = new_aead(h, bs(key, 32), p->nonce_size, &err);
    Slice ct = cipher_aead_seal(aead, h, slice_nil(TYPE_BYTE), bs(nonce, p->nonce_size),
                                buf, bs(ad, 13));
    Slice out = make_bytes(h, p->length + 16);

    testing_b_reset_timer(b);
    for (Int i = 0; i < testing_b_n(b); i++) {
        if (p->open)
            cipher_aead_open(aead, h, slice_sub(out, 0, 0), bs(nonce, p->nonce_size),
                             ct, bs(ad, 13), &err);
        else
            cipher_aead_seal(aead, h, slice_sub(out, 0, 0), bs(nonce, p->nonce_size),
                             buf, bs(ad, 13));
    }
    sink ^= bp(out)[0];
    mem_free(h, buf.p, (size_t)buf.cap, 1);
    mem_free(h, ct.p, (size_t)ct.cap, 1);
    mem_free(h, out.p, (size_t)out.cap, 1);
    mem_free(h, aead.data, 32, 1);
}

static void BenchmarkChacha20Poly1305(TestingB *b) {
    static const Int lengths[] = {64, 1350, 8 * 1024};
    static AeadBench cases[LEN(lengths)][4];
    for (size_t i = 0; i < LEN(lengths); i++) {
        Int l = lengths[i];
        cases[i][0] = (AeadBench){l, CHACHA20POLY1305_NONCE_SIZE, true};
        cases[i][1] = (AeadBench){l, CHACHA20POLY1305_NONCE_SIZE, false};
        cases[i][2] = (AeadBench){l, CHACHA20POLY1305_NONCE_SIZE_X, true};
        cases[i][3] = (AeadBench){l, CHACHA20POLY1305_NONCE_SIZE_X, false};
        for (int k = 0; k < 4; k++) {
            char name[32];
            snprintf(name, sizeof name, "%s-%d%s", k % 2 ? "Seal" : "Open", (int)l,
                     k >= 2 ? "-X" : "");
            testing_b_run(b, str_from_cstr(name),
                          BURROW_FN(TestingBFunc, benchmark_aead, &cases[i][k]));
        }
    }
}

#define TESTS(X)                                                                       \
    X(TestNoOverlap)                                                                   \
    X(TestOverlap)                                                                     \
    X(TestUnaligned)                                                                   \
    X(TestStep)                                                                        \
    X(TestSetCounter)                                                                  \
    X(TestLastBlock)                                                                   \
    X(TestHChaCha20)                                                                   \
    X(TestBurnin)                                                                      \
    X(TestSum)                                                                         \
    X(TestSumUnaligned)                                                                \
    X(TestSumGeneric)                                                                  \
    X(TestSumGenericUnaligned)                                                         \
    X(TestWriteGeneric)                                                                \
    X(TestWriteGenericUnaligned)                                                       \
    X(TestWrite)                                                                       \
    X(TestWriteUnaligned)                                                              \
    X(TestWriteAfterSum)                                                               \
    X(TestVectors)                                                                     \
    X(TestRandom)                                                                      \
    X(TestExampleNewX)                                                                 \
    X(TestErrors)                                                                      \
    X(TestInPlace)                                                                     \
    X(TestCipherStream)                                                                \
    X(BenchmarkChaCha20)                                                               \
    X(Benchmark64)                                                                     \
    X(Benchmark1K)                                                                     \
    X(Benchmark2M)                                                                     \
    X(Benchmark64Unaligned)                                                            \
    X(Benchmark1KUnaligned)                                                            \
    X(Benchmark2MUnaligned)                                                            \
    X(BenchmarkWrite64)                                                                \
    X(BenchmarkWrite1K)                                                                \
    X(BenchmarkWrite2M)                                                                \
    X(BenchmarkWrite64Unaligned)                                                       \
    X(BenchmarkWrite1KUnaligned)                                                       \
    X(BenchmarkWrite2MUnaligned)                                                       \
    X(BenchmarkChacha20Poly1305)

TESTING_MAIN(TESTS)
