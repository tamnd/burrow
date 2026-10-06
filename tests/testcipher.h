/* Derived from Go's src/crypto/internal/cryptotest/block.go, blockmode.go,
 * stream.go, aead.go, boundary.go and implementations.go.
 * Go source: go1.27.1.
 *
 * The checks every block cipher, block mode, stream and AEAD in the standard
 * library has to pass, shared by the cipher tests the way Go shares them
 * through internal/cryptotest. testcipher_block is TestBlock,
 * testcipher_block_mode is TestBlockMode, testcipher_stream is TestStream,
 * testcipher_stream_from_block is TestStreamFromBlock and testcipher_aead is
 * TestAEAD.
 *
 * Go passes closures. Here a maker is a function and a void pointer, and a
 * check that has to see a panic gets a TestcipherOp saying what to call.
 *
 * testcipher_all_implementations is TestAllImplementations for the one package
 * that has more than one implementation so far, AES, and it selects them
 * through the hooks aes_internal.h gives the tests.
 *
 * Copyright 2024 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_TESTS_TESTCIPHER_H
#define BURROW_TESTS_TESTCIPHER_H

#include "testhash.h"

#include "../src/crypto/aes_internal.h"

#include "burrow/burrow.h"
#include "burrow/crypto/cipher.h"
#include "burrow/crypto/subtle.h"
#include "burrow/mem/arena.h"
#include "burrow/pal.h"
#include "burrow/strings.h"

#include <string.h>

/* ------------------------------------------------------------------ helpers */

#define TESTCIPHER_ARENA                                                               \
    Arena ar;                                                                          \
    arena_init(&ar, NULL, 0);                                                          \
    Alloc *a = arena_allocator(&ar)

/* Go's truncateHex: the first 50 bytes in hex, and "..." when there were more. */
static inline Str testcipher_hex(Alloc *a, Slice b) {
    if (b.len <= 50)
        return fmt_sprintf_v(a, "%x", b);
    return fmt_sprintf_v(a, "%x...", slice_sub(b, 0, 50));
}

/* make([]byte, n+1)[0:n], a slice with a byte of room past its end. */
static inline Slice testcipher_short(Alloc *a, Int n) {
    return slice_sub(testhash_bytes(a, n + 1), 0, n);
}

/* What a check that expects a panic calls. fn gets the op back and does the
 * call with whichever of the fields it needs. */
typedef struct TestcipherOp TestcipherOp;
struct TestcipherOp {
    void (*fn)(TestcipherOp *op);
    const void *env;
    Alloc *a;
    Slice dst, src, nonce, ad;
};

static inline bool testcipher_panics(TestcipherOp *op) {
    volatile bool panicked = false;
    BURROW_TRY {
        op->fn(op);
    }
    BURROW_CATCH(r) {
        (void)r;
        panicked = true;
    }
    BURROW_TRY_END;
    return panicked;
}

static inline void testcipher_must_panic(TestingT *t, const char *msg,
                                         TestcipherOp *op) {
    testing_t_helper(t);
    if (!testcipher_panics(op))
        testing_t_errorf_v(t, "function did not panic for %q", str_from_cstr(msg));
}

/* Go's BoundarySlices: size bytes right after an inaccessible page and size
 * bytes right before one, so an access past either end faults. Go unmaps them
 * in a cleanup and this hands back what to release instead. */
typedef struct TestcipherGuard {
    Byte *base;
    int64_t bytes;
} TestcipherGuard;

static inline void testcipher_boundary(TestingT *t, Int size, TestcipherGuard *g,
                                       Slice *start, Slice *end) {
    int64_t page = pal_page_size();
    /* At least one page between the guards, since committing none fails. */
    int64_t mid = (2 * (int64_t)size + page - 1) / page;
    int64_t need = 2 + (mid > 0 ? mid : 1);
    PalErrno err = 0;
    Byte *b = (Byte *)pal_vm_reserve(need * page, &err);
    if (b == NULL)
        testing_t_fatalf_v(t, "mmap failed: %s", pal_errno_string(err));
    if (!pal_vm_commit(b + page, (need - 2) * page, &err))
        testing_t_fatalf_v(t, "mprotect failed: %s", pal_errno_string(err));
    g->base = b;
    g->bytes = need * page;
    *start = slice_from(b + page, size, size, TYPE_BYTE);
    *end = slice_from(b + need * page - page - size, size, size, TYPE_BYTE);
}

static inline void testcipher_release(TestcipherGuard *g) {
    if (g->base != NULL)
        (void)pal_vm_release(g->base, g->bytes, NULL);
    g->base = NULL;
}

/* -------------------------------------------------------------------- Block */

typedef CipherBlock (*TestcipherMakeBlock)(Alloc *a, Slice key, Error *err);

typedef struct TestcipherBlockEnv {
    CipherBlock block;
    Int block_size;
    bool decrypt;
} TestcipherBlockEnv;

static inline void testcipher_crypt(const TestcipherBlockEnv *e, Slice dst, Slice src) {
    if (e->decrypt)
        cipher_block_decrypt(e->block, dst, src);
    else
        cipher_block_encrypt(e->block, dst, src);
}

static inline void testcipher_crypt_op(TestcipherOp *op) {
    testcipher_crypt((const TestcipherBlockEnv *)op->env, op->dst, op->src);
}

static inline void testcipher_cipher_alter_input(void *env, TestingT *t) {
    const TestcipherBlockEnv *e = env;
    TESTCIPHER_ARENA;
    Int bs = e->block_size;
    TesthashRand rng = testhash_new_rand(t);
    Slice src = testhash_bytes(a, bs * 2), before = testhash_bytes(a, bs * 2);
    testhash_read(&rng, src);
    slice_copy(before, src);
    Slice dst = testhash_bytes(a, bs);
    testcipher_crypt(e, dst, src);
    if (!testhash_equal(src, before))
        testing_t_errorf_v(t, "block cipher modified src; got %x, want %x", src,
                           before);
    arena_free(&ar);
}

static inline void testcipher_cipher_aliasing(void *env, TestingT *t) {
    const TestcipherBlockEnv *e = env;
    TESTCIPHER_ARENA;
    Int bs = e->block_size;
    TesthashRand rng = testhash_new_rand(t);
    Slice buff = testhash_bytes(a, bs), expected = testhash_bytes(a, bs);
    testhash_read(&rng, buff);
    testcipher_crypt(e, expected, buff);
    testcipher_crypt(e, buff, buff);
    if (!testhash_equal(buff, expected))
        testing_t_errorf_v(
            t, "block cipher produced different output when dst = src; got %x, want %x",
            buff, expected);
    arena_free(&ar);
}

static inline void testcipher_cipher_out_of_bounds_write(void *env, TestingT *t) {
    const TestcipherBlockEnv *e = env;
    TESTCIPHER_ARENA;
    Int bs = e->block_size;
    TesthashRand rng = testhash_new_rand(t);
    Slice src = testhash_bytes(a, bs);
    testhash_read(&rng, src);

    Slice buff = testhash_bytes(a, bs * 3);
    Int end_of_prefix = bs, start_of_suffix = bs * 2;
    testhash_read(&rng, slice_sub(buff, 0, end_of_prefix));
    testhash_read(&rng, slice_sub(buff, start_of_suffix, buff.len));
    Slice dst = slice_sub(buff, end_of_prefix, start_of_suffix);

    Slice init_prefix = testhash_bytes(a, bs), init_suffix = testhash_bytes(a, bs);
    slice_copy(init_prefix, slice_sub(buff, 0, end_of_prefix));
    slice_copy(init_suffix, slice_sub(buff, start_of_suffix, buff.len));

    testcipher_crypt(e, dst, src);
    Slice suffix = slice_sub(buff, start_of_suffix, buff.len);
    Slice prefix = slice_sub(buff, 0, end_of_prefix);
    if (!testhash_equal(suffix, init_suffix))
        testing_t_errorf_v(t,
                           "block cipher did out of bounds write after end of dst "
                           "slice; got %x, want %x",
                           suffix, init_suffix);
    if (!testhash_equal(prefix, init_prefix))
        testing_t_errorf_v(t,
                           "block cipher did out of bounds write before beginning of "
                           "dst slice; got %x, want %x",
                           prefix, init_prefix);

    dst = slice_sub(buff, end_of_prefix, buff.len);
    testcipher_crypt(e, dst, src);
    if (!testhash_equal(suffix, init_suffix))
        testing_t_errorf_v(t,
                           "block cipher modified dst past BlockSize bytes; got %x, "
                           "want %x",
                           suffix, init_suffix);
    arena_free(&ar);
}

static inline void testcipher_cipher_out_of_bounds_read(void *env, TestingT *t) {
    const TestcipherBlockEnv *e = env;
    TESTCIPHER_ARENA;
    Int bs = e->block_size;
    TesthashRand rng = testhash_new_rand(t);
    Slice src = testhash_bytes(a, bs);
    testhash_read(&rng, src);
    Slice expected = testhash_bytes(a, bs);
    testcipher_crypt(e, expected, src);

    Slice buff = testhash_bytes(a, bs * 3);
    Int end_of_prefix = bs, start_of_suffix = bs * 2;
    slice_copy(slice_sub(buff, end_of_prefix, start_of_suffix), src);
    testhash_read(&rng, slice_sub(buff, 0, end_of_prefix));
    testhash_read(&rng, slice_sub(buff, start_of_suffix, buff.len));

    Slice test_dst = testhash_bytes(a, bs);
    testcipher_crypt(e, test_dst, slice_sub(buff, end_of_prefix, start_of_suffix));
    if (!testhash_equal(test_dst, expected))
        testing_t_errorf_v(t,
                           "block cipher affected by data outside of src slice bounds; "
                           "got %x, want %x",
                           test_dst, expected);

    testcipher_crypt(e, test_dst, slice_sub(buff, end_of_prefix, buff.len));
    if (!testhash_equal(test_dst, expected))
        testing_t_errorf_v(t,
                           "block cipher affected by src data beyond BlockSize bytes; "
                           "got %x, want %x",
                           slice_sub(buff, start_of_suffix, buff.len), expected);
    arena_free(&ar);
}

static inline void testcipher_cipher_non_zero_dst(void *env, TestingT *t) {
    const TestcipherBlockEnv *e = env;
    TESTCIPHER_ARENA;
    Int bs = e->block_size;
    TesthashRand rng = testhash_new_rand(t);
    Slice src = testhash_bytes(a, bs);
    testhash_read(&rng, src);
    Slice expected = testhash_bytes(a, bs);
    testcipher_crypt(e, expected, src);

    Slice dst = testhash_bytes(a, bs * 2);
    testhash_read(&rng, dst);
    expected = slice_append_slice(a, expected, slice_sub(dst, bs, dst.len));

    testcipher_crypt(e, dst, src);
    if (!testhash_equal(dst, expected))
        testing_t_errorf_v(t,
                           "block cipher behavior differs when given non-zero dst; got "
                           "%x, want %x",
                           dst, expected);
    arena_free(&ar);
}

static inline void testcipher_cipher_buffer_overlap(void *env, TestingT *t) {
    const TestcipherBlockEnv *e = env;
    TESTCIPHER_ARENA;
    Int bs = e->block_size;
    TesthashRand rng = testhash_new_rand(t);
    Slice buff = testhash_bytes(a, bs * 2);
    testhash_read(&rng, buff);

    TestcipherOp op = {testcipher_crypt_op, e, a, {0}, {0}, {0}, {0}};
    op.src = slice_sub(buff, 0, bs);
    op.dst = slice_sub(buff, 1, bs + 1);
    testcipher_must_panic(t, "invalid buffer overlap", &op);

    op.src = slice_sub(buff, 0, bs);
    op.dst = slice_sub(buff, bs - 1, 2 * bs - 1);
    testcipher_must_panic(t, "invalid buffer overlap", &op);

    op.src = slice_sub(buff, bs - 1, 2 * bs - 1);
    op.dst = slice_sub(buff, 0, bs);
    testcipher_must_panic(t, "invalid buffer overlap", &op);
    arena_free(&ar);
}

static inline void testcipher_cipher_short_block(void *env, TestingT *t) {
    const TestcipherBlockEnv *e = env;
    TESTCIPHER_ARENA;
    Int bs = e->block_size;
    TestcipherOp op = {testcipher_crypt_op, e, a, {0}, {0}, {0}, {0}};

    op.dst = testcipher_short(a, bs);
    op.src = testcipher_short(a, bs - 1);
    testcipher_must_panic(t, "input not full block", &op);
    op.dst = testcipher_short(a, bs - 1);
    op.src = testcipher_short(a, bs);
    testcipher_must_panic(t, "output not full block", &op);

    op.dst = testcipher_short(a, 1);
    op.src = testcipher_short(a, 1);
    testcipher_must_panic(t, "input not full block", &op);
    op.dst = testcipher_short(a, 100);
    op.src = testcipher_short(a, 1);
    testcipher_must_panic(t, "input not full block", &op);
    op.dst = testcipher_short(a, 1);
    op.src = testcipher_short(a, 100);
    testcipher_must_panic(t, "output not full block", &op);
    arena_free(&ar);
}

/* Go's testCipher, for one direction. */
static inline void testcipher_cipher(void *env, TestingT *t) {
    testing_t_run(t, BURROW_S("AlterInput"),
                  BURROW_FN(TestingTFunc, testcipher_cipher_alter_input, env));
    testing_t_run(t, BURROW_S("Aliasing"),
                  BURROW_FN(TestingTFunc, testcipher_cipher_aliasing, env));
    testing_t_run(t, BURROW_S("OutOfBoundsWrite"),
                  BURROW_FN(TestingTFunc, testcipher_cipher_out_of_bounds_write, env));
    testing_t_run(t, BURROW_S("OutOfBoundsRead"),
                  BURROW_FN(TestingTFunc, testcipher_cipher_out_of_bounds_read, env));
    testing_t_run(t, BURROW_S("NonZeroDst"),
                  BURROW_FN(TestingTFunc, testcipher_cipher_non_zero_dst, env));
    testing_t_run(t, BURROW_S("BufferOverlap"),
                  BURROW_FN(TestingTFunc, testcipher_cipher_buffer_overlap, env));
    testing_t_run(t, BURROW_S("ShortBlock"),
                  BURROW_FN(TestingTFunc, testcipher_cipher_short_block, env));
}

static inline void testcipher_block_roundtrip(void *env, TestingT *t) {
    const TestcipherBlockEnv *e = env;
    TESTCIPHER_ARENA;
    Int bs = e->block_size;
    TesthashRand rng = testhash_new_rand(t);

    Slice before = testhash_bytes(a, bs), ciphertext = testhash_bytes(a, bs),
          after = testhash_bytes(a, bs);
    testhash_read(&rng, before);
    cipher_block_encrypt(e->block, ciphertext, before);
    cipher_block_decrypt(e->block, after, ciphertext);
    if (!testhash_equal(after, before))
        testing_t_errorf_v(t,
                           "plaintext is different after an encrypt/decrypt cycle; got "
                           "%x, want %x",
                           after, before);

    before = testhash_bytes(a, bs);
    Slice plaintext = testhash_bytes(a, bs);
    after = testhash_bytes(a, bs);
    testhash_read(&rng, before);
    cipher_block_decrypt(e->block, plaintext, before);
    cipher_block_encrypt(e->block, after, plaintext);
    if (!testhash_equal(after, before))
        testing_t_errorf_v(t,
                           "ciphertext is different after a decrypt/encrypt cycle; got "
                           "%x, want %x",
                           after, before);
    arena_free(&ar);
}

/* Go's TestBlock. */
static inline void testcipher_block(TestingT *t, Int key_size, TestcipherMakeBlock mb) {
    TESTCIPHER_ARENA;
    Slice key = testhash_bytes(a, key_size);
    TesthashRand rng = testhash_new_rand(t);
    testhash_read(&rng, key);
    testing_t_logf_v(t, "Cipher key: 0x%x", key);

    Error err = BURROW_NO_ERROR;
    CipherBlock block = mb(a, key, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%v", err);

    TestcipherBlockEnv enc = {block, cipher_block_block_size(block), false};
    TestcipherBlockEnv dec = {block, enc.block_size, true};
    testing_t_run(t, BURROW_S("Encryption"),
                  BURROW_FN(TestingTFunc, testcipher_cipher, &enc));
    testing_t_run(t, BURROW_S("Decryption"),
                  BURROW_FN(TestingTFunc, testcipher_cipher, &dec));
    testing_t_run(t, BURROW_S("Roundtrip"),
                  BURROW_FN(TestingTFunc, testcipher_block_roundtrip, &enc));
    arena_free(&ar);
}

/* ---------------------------------------------------------------- BlockMode */

typedef CipherBlockMode (*TestcipherMakeBlockMode)(Alloc *a, CipherBlock b, Slice iv);

typedef struct TestcipherModeEnv {
    TestcipherMakeBlockMode bm;
    TestcipherMakeBlockMode enc, dec;
    CipherBlock b;
    Slice iv;
    Int block_size;
} TestcipherModeEnv;

static inline void testcipher_mode_crypt_op(TestcipherOp *op) {
    const TestcipherModeEnv *e = op->env;
    cipher_block_mode_crypt_blocks(e->bm(op->a, e->b, e->iv), op->dst, op->src);
}

static inline void testcipher_mode_make_op(TestcipherOp *op) {
    const TestcipherModeEnv *e = op->env;
    (void)e->bm(op->a, e->b, op->nonce);
}

static inline void testcipher_mode_crypt(const TestcipherModeEnv *e, Alloc *a,
                                         Slice dst, Slice src) {
    cipher_block_mode_crypt_blocks(e->bm(a, e->b, e->iv), dst, src);
}

static inline void testcipher_mode_wrong_iv_len(void *env, TestingT *t) {
    const TestcipherModeEnv *e = env;
    TESTCIPHER_ARENA;
    TestcipherOp op = {testcipher_mode_make_op, e, a, {0}, {0}, {0}, {0}};
    op.nonce = testhash_bytes(a, cipher_block_block_size(e->b) + 1);
    testcipher_must_panic(t, "IV length must equal block size", &op);
    arena_free(&ar);
}

static inline void testcipher_mode_empty_input(void *env, TestingT *t) {
    const TestcipherModeEnv *e = env;
    TESTCIPHER_ARENA;
    Int bs = e->block_size;
    TesthashRand rng = testhash_new_rand(t);
    Slice src = testhash_bytes(a, bs), dst = testhash_bytes(a, bs);
    testhash_read(&rng, dst);
    Slice before = testhash_clone(a, dst);
    testcipher_mode_crypt(e, a, dst, slice_sub(src, 0, 0));
    if (!testhash_equal(dst, before))
        testing_t_errorf_v(
            t, "CryptBlocks modified dst on empty input; got %x, want %x", dst, before);
    arena_free(&ar);
}

static inline void testcipher_mode_alter_input(void *env, TestingT *t) {
    const TestcipherModeEnv *e = env;
    TESTCIPHER_ARENA;
    Int bs = e->block_size;
    TesthashRand rng = testhash_new_rand(t);
    Slice src = testhash_bytes(a, bs * 2), dst = testhash_bytes(a, bs * 2),
          before = testhash_bytes(a, bs * 2);
    Int lengths[] = {0, bs, bs * 2};
    for (size_t i = 0; i < sizeof lengths / sizeof lengths[0]; i++) {
        testhash_read(&rng, src);
        slice_copy(before, src);
        testcipher_mode_crypt(e, a, slice_sub(dst, 0, lengths[i]),
                              slice_sub(src, 0, lengths[i]));
        if (!testhash_equal(src, before))
            testing_t_errorf_v(t, "CryptBlocks modified src; got %x, want %x", src,
                               before);
    }
    arena_free(&ar);
}

static inline void testcipher_mode_aliasing(void *env, TestingT *t) {
    const TestcipherModeEnv *e = env;
    TESTCIPHER_ARENA;
    Int bs = e->block_size;
    TesthashRand rng = testhash_new_rand(t);
    Slice buff = testhash_bytes(a, bs * 2), expected = testhash_bytes(a, bs * 2);
    Int lengths[] = {0, bs, bs * 2};
    for (size_t i = 0; i < sizeof lengths / sizeof lengths[0]; i++) {
        Int n = lengths[i];
        testhash_read(&rng, buff);
        testcipher_mode_crypt(e, a, slice_sub(expected, 0, n), slice_sub(buff, 0, n));
        testcipher_mode_crypt(e, a, slice_sub(buff, 0, n), slice_sub(buff, 0, n));
        if (!testhash_equal(slice_sub(buff, 0, n), slice_sub(expected, 0, n)))
            testing_t_errorf_v(t,
                               "block cipher produced different output when dst = src; "
                               "got %x, want %x",
                               slice_sub(buff, 0, n), slice_sub(expected, 0, n));
    }
    arena_free(&ar);
}

static inline void testcipher_mode_out_of_bounds_write(void *env, TestingT *t) {
    const TestcipherModeEnv *e = env;
    TESTCIPHER_ARENA;
    Int bs = e->block_size;
    TesthashRand rng = testhash_new_rand(t);
    Slice src = testhash_bytes(a, bs);
    testhash_read(&rng, src);

    Slice buff = testhash_bytes(a, bs * 3);
    Int end_of_prefix = bs, start_of_suffix = bs * 2;
    testhash_read(&rng, slice_sub(buff, 0, end_of_prefix));
    testhash_read(&rng, slice_sub(buff, start_of_suffix, buff.len));
    Slice dst = slice_sub(buff, end_of_prefix, start_of_suffix);

    Slice init_prefix = testhash_bytes(a, bs), init_suffix = testhash_bytes(a, bs);
    slice_copy(init_prefix, slice_sub(buff, 0, end_of_prefix));
    slice_copy(init_suffix, slice_sub(buff, start_of_suffix, buff.len));
    Slice prefix = slice_sub(buff, 0, end_of_prefix);
    Slice suffix = slice_sub(buff, start_of_suffix, buff.len);

    testcipher_mode_crypt(e, a, dst, src);
    if (!testhash_equal(suffix, init_suffix))
        testing_t_errorf_v(t,
                           "block cipher did out of bounds write after end of dst "
                           "slice; got %x, want %x",
                           suffix, init_suffix);
    if (!testhash_equal(prefix, init_prefix))
        testing_t_errorf_v(t,
                           "block cipher did out of bounds write before beginning of "
                           "dst slice; got %x, want %x",
                           prefix, init_prefix);

    dst = slice_sub(buff, end_of_prefix, buff.len);
    testcipher_mode_crypt(e, a, dst, src);
    if (!testhash_equal(suffix, init_suffix))
        testing_t_errorf_v(t, "CryptBlocks modified dst past len(src); got %x, want %x",
                           suffix, init_suffix);

    src = testhash_bytes(a, bs * 3);
    testhash_read(&rng, src);
    TestcipherOp op = {testcipher_mode_crypt_op, e, a, dst, src, {0}, {0}};
    testcipher_must_panic(t, "output smaller than input", &op);
    if (!testhash_equal(suffix, init_suffix))
        testing_t_errorf_v(t,
                           "block cipher did out of bounds write after end of dst "
                           "slice; got %x, want %x",
                           suffix, init_suffix);
    if (!testhash_equal(prefix, init_prefix))
        testing_t_errorf_v(t,
                           "block cipher did out of bounds write before beginning of "
                           "dst slice; got %x, want %x",
                           prefix, init_prefix);
    arena_free(&ar);
}

static inline void testcipher_mode_out_of_bounds_read(void *env, TestingT *t) {
    const TestcipherModeEnv *e = env;
    TESTCIPHER_ARENA;
    Int bs = e->block_size;
    TesthashRand rng = testhash_new_rand(t);
    Slice src = testhash_bytes(a, bs);
    testhash_read(&rng, src);
    Slice expected = testhash_bytes(a, bs);
    testcipher_mode_crypt(e, a, expected, src);

    Slice buff = testhash_bytes(a, bs * 3);
    Int end_of_prefix = bs, start_of_suffix = bs * 2;
    slice_copy(slice_sub(buff, end_of_prefix, start_of_suffix), src);
    testhash_read(&rng, slice_sub(buff, 0, end_of_prefix));
    testhash_read(&rng, slice_sub(buff, start_of_suffix, buff.len));

    Slice test_dst = testhash_bytes(a, bs);
    testcipher_mode_crypt(e, a, test_dst,
                          slice_sub(buff, end_of_prefix, start_of_suffix));
    if (!testhash_equal(test_dst, expected))
        testing_t_errorf_v(t,
                           "CryptBlocks affected by data outside of src slice bounds; "
                           "got %x, want %x",
                           test_dst, expected);
    arena_free(&ar);
}

static inline void testcipher_mode_buffer_overlap(void *env, TestingT *t) {
    const TestcipherModeEnv *e = env;
    TESTCIPHER_ARENA;
    Int bs = e->block_size;
    TesthashRand rng = testhash_new_rand(t);
    Slice buff = testhash_bytes(a, bs * 2);
    testhash_read(&rng, buff);

    TestcipherOp op = {testcipher_mode_crypt_op, e, a, {0}, {0}, {0}, {0}};
    op.src = slice_sub(buff, 0, bs);
    op.dst = slice_sub(buff, 1, bs + 1);
    testcipher_must_panic(t, "invalid buffer overlap", &op);

    op.src = slice_sub(buff, 0, bs);
    op.dst = slice_sub(buff, bs - 1, 2 * bs - 1);
    testcipher_must_panic(t, "invalid buffer overlap", &op);

    op.src = slice_sub(buff, bs - 1, 2 * bs - 1);
    op.dst = slice_sub(buff, 0, bs);
    testcipher_must_panic(t, "invalid buffer overlap", &op);
    arena_free(&ar);
}

static inline void testcipher_mode_partial_blocks(void *env, TestingT *t) {
    const TestcipherModeEnv *e = env;
    TESTCIPHER_ARENA;
    Int bs = e->block_size;
    Int sizes[] = {bs - 1, bs + 1, 2 * bs - 1, 2 * bs + 1};
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        TestcipherOp op = {testcipher_mode_crypt_op, e, a, {0}, {0}, {0}, {0}};
        op.src = testhash_bytes(a, sizes[i]);
        op.dst = testhash_bytes(a, 3 * bs);
        testcipher_must_panic(t, "input not full blocks", &op);
    }
    arena_free(&ar);
}

static inline void testcipher_mode_keep_state(void *env, TestingT *t) {
    const TestcipherModeEnv *e = env;
    TESTCIPHER_ARENA;
    Int bs = e->block_size;
    TesthashRand rng = testhash_new_rand(t);
    Slice src = testhash_bytes(a, bs * 4), serial = testhash_bytes(a, bs * 4),
          composite = testhash_bytes(a, bs * 4);
    testhash_read(&rng, src);

    Int length = 2 * bs;
    CipherBlockMode block = e->bm(a, e->b, e->iv);
    cipher_block_mode_crypt_blocks(block, serial, slice_sub(src, 0, length));
    cipher_block_mode_crypt_blocks(block, slice_sub(serial, length, serial.len),
                                   slice_sub(src, length, src.len));
    testcipher_mode_crypt(e, a, composite, src);
    if (!testhash_equal(serial, composite))
        testing_t_errorf_v(t,
                           "two successive CryptBlocks calls returned a different "
                           "result than a single one; got %x, want %x",
                           serial, composite);
    arena_free(&ar);
}

/* Go's testBlockMode, with env->bm the mode under test. */
static inline void testcipher_mode_one(void *env, TestingT *t) {
    testing_t_run(t, BURROW_S("WrongIVLen"),
                  BURROW_FN(TestingTFunc, testcipher_mode_wrong_iv_len, env));
    testing_t_run(t, BURROW_S("EmptyInput"),
                  BURROW_FN(TestingTFunc, testcipher_mode_empty_input, env));
    testing_t_run(t, BURROW_S("AlterInput"),
                  BURROW_FN(TestingTFunc, testcipher_mode_alter_input, env));
    testing_t_run(t, BURROW_S("Aliasing"),
                  BURROW_FN(TestingTFunc, testcipher_mode_aliasing, env));
    testing_t_run(t, BURROW_S("OutOfBoundsWrite"),
                  BURROW_FN(TestingTFunc, testcipher_mode_out_of_bounds_write, env));
    testing_t_run(t, BURROW_S("OutOfBoundsRead"),
                  BURROW_FN(TestingTFunc, testcipher_mode_out_of_bounds_read, env));
    testing_t_run(t, BURROW_S("BufferOverlap"),
                  BURROW_FN(TestingTFunc, testcipher_mode_buffer_overlap, env));
    testing_t_run(t, BURROW_S("PartialBlocks"),
                  BURROW_FN(TestingTFunc, testcipher_mode_partial_blocks, env));
    testing_t_run(t, BURROW_S("KeepState"),
                  BURROW_FN(TestingTFunc, testcipher_mode_keep_state, env));
}

static inline void testcipher_mode_roundtrip(void *env, TestingT *t) {
    const TestcipherModeEnv *e = env;
    TESTCIPHER_ARENA;
    TesthashRand rng = testhash_new_rand(t);
    Int bs = cipher_block_mode_block_size(e->enc(a, e->b, e->iv));
    Int dec_bs = cipher_block_mode_block_size(e->dec(a, e->b, e->iv));
    if (dec_bs != bs)
        testing_t_errorf_v(t,
                           "decryption blocksize different than encryption's; got %d, "
                           "want %d",
                           dec_bs, bs);
    Slice before = testhash_bytes(a, bs * 2), dst = testhash_bytes(a, bs * 2),
          after = testhash_bytes(a, bs * 2);
    testhash_read(&rng, before);
    cipher_block_mode_crypt_blocks(e->enc(a, e->b, e->iv), dst, before);
    cipher_block_mode_crypt_blocks(e->dec(a, e->b, e->iv), after, dst);
    if (!testhash_equal(after, before))
        testing_t_errorf_v(t,
                           "plaintext is different after an encrypt/decrypt cycle; got "
                           "%x, want %x",
                           after, before);
    arena_free(&ar);
}

/* Go's TestBlockMode. */
static inline void testcipher_block_mode(TestingT *t, CipherBlock block,
                                         TestcipherMakeBlockMode make_encrypter,
                                         TestcipherMakeBlockMode make_decrypter) {
    TESTCIPHER_ARENA;
    TesthashRand rng = testhash_new_rand(t);
    Slice iv = testhash_bytes(a, cipher_block_block_size(block));
    testhash_read(&rng, iv);

    TestcipherModeEnv enc = {
        make_encrypter, make_encrypter, make_decrypter, block, iv, 0};
    enc.block_size = cipher_block_mode_block_size(make_encrypter(a, block, iv));
    TestcipherModeEnv dec = enc;
    dec.bm = make_decrypter;
    dec.block_size = cipher_block_mode_block_size(make_decrypter(a, block, iv));

    testing_t_run(t, BURROW_S("Encryption"),
                  BURROW_FN(TestingTFunc, testcipher_mode_one, &enc));
    testing_t_run(t, BURROW_S("Decryption"),
                  BURROW_FN(TestingTFunc, testcipher_mode_one, &dec));
    testing_t_run(t, BURROW_S("Roundtrip"),
                  BURROW_FN(TestingTFunc, testcipher_mode_roundtrip, &enc));
    arena_free(&ar);
}

/* ------------------------------------------------------------------- Stream */

typedef CipherStream (*TestcipherMakeStream)(Alloc *a, void *env);

static const Int testcipher_buf_lens[] = {0,  1,  3,  4,  8,    10,  15,
                                          16, 20, 32, 50, 4096, 5000};
#define TESTCIPHER_NBUF_LENS                                                           \
    ((Int)(sizeof testcipher_buf_lens / sizeof testcipher_buf_lens[0]))
#define TESTCIPHER_BUF_CAP 10000

typedef struct TestcipherStreamEnv {
    TestcipherMakeStream ms;
    void *env;
    /* For the subtests that take one length, and for the panics. */
    Int length;
    Slice plaintext, ciphertext, buff;
} TestcipherStreamEnv;

static inline CipherStream testcipher_ms(const TestcipherStreamEnv *e, Alloc *a) {
    return e->ms(a, e->env);
}

static inline void testcipher_stream_xor_op(TestcipherOp *op) {
    const TestcipherStreamEnv *e = op->env;
    cipher_stream_xor_key_stream(testcipher_ms(e, op->a), op->dst, op->src);
}

static inline Str testcipher_buff_name(Alloc *a, Int length) {
    return fmt_sprintf_v(a, "BuffLength=%d", length);
}

static inline void testcipher_stream_roundtrip_one(void *env, TestingT *t) {
    const TestcipherStreamEnv *e = env;
    TESTCIPHER_ARENA;
    TesthashRand rng = testhash_new_rand(t);
    Slice plaintext = testhash_bytes(a, e->length);
    testhash_read(&rng, plaintext);
    Slice ciphertext = testhash_bytes(a, e->length);
    Slice decrypted = testhash_bytes(a, e->length);
    cipher_stream_xor_key_stream(testcipher_ms(e, a), ciphertext, plaintext);
    cipher_stream_xor_key_stream(testcipher_ms(e, a), decrypted, ciphertext);
    if (!testhash_equal(decrypted, plaintext))
        testing_t_errorf_v(t,
                           "plaintext is different after an encrypt/decrypt cycle; got "
                           "%s, want %s",
                           testcipher_hex(a, decrypted), testcipher_hex(a, plaintext));
    arena_free(&ar);
}

static inline void testcipher_stream_direct_xor_one(void *env, TestingT *t) {
    const TestcipherStreamEnv *e = env;
    TESTCIPHER_ARENA;
    TesthashRand rng = testhash_new_rand(t);
    Slice plaintext = testhash_bytes(a, e->length);
    testhash_read(&rng, plaintext);
    Slice stream = testhash_bytes(a, e->length), direct = testhash_bytes(a, e->length);
    cipher_stream_xor_key_stream(testcipher_ms(e, a), stream, stream);
    subtle_xor_bytes(direct, stream, plaintext);
    Slice ciphertext = testhash_bytes(a, e->length);
    cipher_stream_xor_key_stream(testcipher_ms(e, a), ciphertext, plaintext);
    if (!testhash_equal(ciphertext, direct))
        testing_t_errorf_v(t, "xor semantics were not preserved; got %s, want %s",
                           testcipher_hex(a, ciphertext), testcipher_hex(a, direct));
    arena_free(&ar);
}

static inline void testcipher_stream_each_length(TestingT *t,
                                                 const TestcipherStreamEnv *e,
                                                 void (*fn)(void *, TestingT *)) {
    TESTCIPHER_ARENA;
    for (Int i = 0; i < TESTCIPHER_NBUF_LENS; i++) {
        TestcipherStreamEnv one = *e;
        one.length = testcipher_buf_lens[i];
        testing_t_run(t, testcipher_buff_name(a, one.length),
                      BURROW_FN(TestingTFunc, fn, &one));
    }
    arena_free(&ar);
}

static inline void testcipher_stream_roundtrip(void *env, TestingT *t) {
    testcipher_stream_each_length(t, env, testcipher_stream_roundtrip_one);
}

static inline void testcipher_stream_direct_xor(void *env, TestingT *t) {
    testcipher_stream_each_length(t, env, testcipher_stream_direct_xor_one);
}

static inline void testcipher_stream_xor_semantics(void *env, TestingT *t) {
    if (strings_contains(testing_t_name(t), BURROW_S("TestCFBStream")))
        testing_t_skip_v(t, "CFB implements cipher.Stream but does not follow XOR "
                            "semantics");
    testing_t_run(t, BURROW_S("Roundtrip"),
                  BURROW_FN(TestingTFunc, testcipher_stream_roundtrip, env));
    testing_t_run(t, BURROW_S("DirectXOR"),
                  BURROW_FN(TestingTFunc, testcipher_stream_direct_xor, env));
}

static inline void testcipher_stream_empty_input(void *env, TestingT *t) {
    const TestcipherStreamEnv *e = env;
    TESTCIPHER_ARENA;
    TesthashRand rng = testhash_new_rand(t);
    Slice src = testhash_bytes(a, 100), dst = testhash_bytes(a, 100);
    testhash_read(&rng, dst);
    Slice before = testhash_clone(a, dst);
    cipher_stream_xor_key_stream(testcipher_ms(e, a), dst, slice_sub(src, 0, 0));
    if (!testhash_equal(dst, before))
        testing_t_errorf_v(t,
                           "XORKeyStream modified dst on empty input; got %s, want %s",
                           testcipher_hex(a, dst), testcipher_hex(a, before));
    arena_free(&ar);
}

static inline void testcipher_stream_alter_input_one(void *env, TestingT *t) {
    const TestcipherStreamEnv *e = env;
    TESTCIPHER_ARENA;
    Slice src = e->plaintext, dst = e->ciphertext, before = e->buff;
    slice_copy(before, src);
    cipher_stream_xor_key_stream(testcipher_ms(e, a), slice_sub(dst, 0, e->length),
                                 slice_sub(src, 0, e->length));
    if (!testhash_equal(src, before))
        testing_t_errorf_v(t, "XORKeyStream modified src; got %s, want %s",
                           testcipher_hex(a, src), testcipher_hex(a, before));
    arena_free(&ar);
}

static inline void testcipher_stream_alter_input(void *env, TestingT *t) {
    TESTCIPHER_ARENA;
    TesthashRand rng = testhash_new_rand(t);
    TestcipherStreamEnv e = *(const TestcipherStreamEnv *)env;
    e.plaintext = testhash_bytes(a, TESTCIPHER_BUF_CAP);
    e.ciphertext = testhash_bytes(a, TESTCIPHER_BUF_CAP);
    e.buff = testhash_bytes(a, TESTCIPHER_BUF_CAP);
    testhash_read(&rng, e.plaintext);
    testcipher_stream_each_length(t, &e, testcipher_stream_alter_input_one);
    arena_free(&ar);
}

static inline void testcipher_stream_aliasing(void *env, TestingT *t) {
    const TestcipherStreamEnv *e = env;
    TESTCIPHER_ARENA;
    TesthashRand rng = testhash_new_rand(t);
    Slice buff = testhash_bytes(a, TESTCIPHER_BUF_CAP);
    Slice expected = testhash_bytes(a, TESTCIPHER_BUF_CAP);
    for (Int i = 0; i < TESTCIPHER_NBUF_LENS; i++) {
        Int n = testcipher_buf_lens[i];
        testhash_read(&rng, buff);
        cipher_stream_xor_key_stream(testcipher_ms(e, a), slice_sub(expected, 0, n),
                                     slice_sub(buff, 0, n));
        cipher_stream_xor_key_stream(testcipher_ms(e, a), slice_sub(buff, 0, n),
                                     slice_sub(buff, 0, n));
        if (!testhash_equal(slice_sub(buff, 0, n), slice_sub(expected, 0, n)))
            testing_t_errorf_v(t,
                               "block cipher produced different output when dst = src; "
                               "got %x, want %x",
                               slice_sub(buff, 0, n), slice_sub(expected, 0, n));
    }
    arena_free(&ar);
}

static inline void testcipher_stream_out_of_bounds_write_one(void *env, TestingT *t) {
    const TestcipherStreamEnv *e = env;
    TESTCIPHER_ARENA;
    TestcipherOp op = {testcipher_stream_xor_op, e, a, {0}, {0}, {0}, {0}};
    op.dst = slice_sub(e->ciphertext, 0, e->length);
    op.src = e->plaintext;
    testcipher_must_panic(t, "output smaller than input", &op);
    Slice got = slice_sub(e->ciphertext, e->length, e->ciphertext.len);
    Slice want = slice_sub(e->plaintext, e->length, e->plaintext.len);
    if (!testhash_equal(got, want))
        testing_t_errorf_v(t, "XORKeyStream did out of bounds write; got %s, want %s",
                           testcipher_hex(a, got), testcipher_hex(a, want));
    arena_free(&ar);
}

static inline void testcipher_stream_out_of_bounds_write(void *env, TestingT *t) {
    TESTCIPHER_ARENA;
    TesthashRand rng = testhash_new_rand(t);
    TestcipherStreamEnv e = *(const TestcipherStreamEnv *)env;
    e.plaintext = testhash_bytes(a, TESTCIPHER_BUF_CAP);
    e.ciphertext = testhash_bytes(a, TESTCIPHER_BUF_CAP);
    testhash_read(&rng, e.plaintext);
    for (Int i = 0; i < TESTCIPHER_NBUF_LENS; i++) {
        slice_copy(e.ciphertext, e.plaintext);
        e.length = testcipher_buf_lens[i];
        testing_t_run(
            t, testcipher_buff_name(a, e.length),
            BURROW_FN(TestingTFunc, testcipher_stream_out_of_bounds_write_one, &e));
    }
    arena_free(&ar);
}

static inline void testcipher_stream_buffer_overlap_one(void *env, TestingT *t) {
    const TestcipherStreamEnv *e = env;
    TESTCIPHER_ARENA;
    Int n = e->length;
    TestcipherOp op = {testcipher_stream_xor_op, e, a, {0}, {0}, {0}, {0}};
    op.src = slice_sub(e->buff, 0, n);
    op.dst = slice_sub(e->buff, 1, n + 1);
    testcipher_must_panic(t, "invalid buffer overlap", &op);

    op.src = slice_sub(e->buff, 0, n);
    op.dst = slice_sub(e->buff, n - 1, 2 * n - 1);
    testcipher_must_panic(t, "invalid buffer overlap", &op);

    op.src = slice_sub(e->buff, n - 1, 2 * n - 1);
    op.dst = slice_sub(e->buff, 0, n);
    testcipher_must_panic(t, "invalid buffer overlap", &op);
    arena_free(&ar);
}

static inline void testcipher_stream_buffer_overlap(void *env, TestingT *t) {
    TESTCIPHER_ARENA;
    TesthashRand rng = testhash_new_rand(t);
    TestcipherStreamEnv e = *(const TestcipherStreamEnv *)env;
    e.buff = testhash_bytes(a, TESTCIPHER_BUF_CAP);
    testhash_read(&rng, e.buff);
    for (Int i = 0; i < TESTCIPHER_NBUF_LENS; i++) {
        e.length = testcipher_buf_lens[i];
        if (e.length == 0 || e.length == 1)
            continue;
        testing_t_run(
            t, testcipher_buff_name(a, e.length),
            BURROW_FN(TestingTFunc, testcipher_stream_buffer_overlap_one, &e));
    }
    arena_free(&ar);
}

static inline void testcipher_stream_keep_state(void *env, TestingT *t) {
    const TestcipherStreamEnv *e = env;
    TESTCIPHER_ARENA;
    TesthashRand rng = testhash_new_rand(t);
    Slice plaintext = testhash_bytes(a, TESTCIPHER_BUF_CAP);
    testhash_read(&rng, plaintext);
    Slice ciphertext = testhash_bytes(a, TESTCIPHER_BUF_CAP);
    cipher_stream_xor_key_stream(testcipher_ms(e, a), ciphertext, plaintext);

    for (Int s = 0; s < TESTCIPHER_NBUF_LENS; s++) {
        Int step = testcipher_buf_lens[s];
        if (step == 0)
            continue;
        Slice dst = testhash_bytes(a, TESTCIPHER_BUF_CAP);
        CipherStream stream = testcipher_ms(e, a);
        Int i = 0;
        for (; i + step < plaintext.len; i += step)
            cipher_stream_xor_key_stream(stream, slice_sub(dst, i, dst.len),
                                         slice_sub(plaintext, i, i + step));
        cipher_stream_xor_key_stream(stream, slice_sub(dst, i, dst.len),
                                     slice_sub(plaintext, i, plaintext.len));
        if (!testhash_equal(dst, ciphertext))
            testing_t_errorf_v(t,
                               "step %d: successive XORKeyStream calls returned a "
                               "different result than a single one; got %s, want %s",
                               step, testcipher_hex(a, dst),
                               testcipher_hex(a, ciphertext));
    }
    arena_free(&ar);
}

/* Go's TestStream. ms makes a fresh stream from env each time it is called. */
static inline void testcipher_stream(TestingT *t, TestcipherMakeStream ms, void *env) {
    TestcipherStreamEnv e = {ms, env, 0, {0}, {0}, {0}};
    testing_t_run(t, BURROW_S("XORSemantics"),
                  BURROW_FN(TestingTFunc, testcipher_stream_xor_semantics, &e));
    testing_t_run(t, BURROW_S("EmptyInput"),
                  BURROW_FN(TestingTFunc, testcipher_stream_empty_input, &e));
    testing_t_run(t, BURROW_S("AlterInput"),
                  BURROW_FN(TestingTFunc, testcipher_stream_alter_input, &e));
    testing_t_run(t, BURROW_S("Aliasing"),
                  BURROW_FN(TestingTFunc, testcipher_stream_aliasing, &e));
    testing_t_run(t, BURROW_S("OutOfBoundsWrite"),
                  BURROW_FN(TestingTFunc, testcipher_stream_out_of_bounds_write, &e));
    testing_t_run(t, BURROW_S("BufferOverlap"),
                  BURROW_FN(TestingTFunc, testcipher_stream_buffer_overlap, &e));
    testing_t_run(t, BURROW_S("KeepState"),
                  BURROW_FN(TestingTFunc, testcipher_stream_keep_state, &e));
}

typedef CipherStream (*TestcipherStreamMode)(Alloc *a, CipherBlock b, Slice iv);

typedef struct TestcipherFromBlockEnv {
    CipherBlock block;
    TestcipherStreamMode mode;
    Slice iv;
} TestcipherFromBlockEnv;

static inline CipherStream testcipher_from_block_make(Alloc *a, void *env) {
    const TestcipherFromBlockEnv *e = env;
    return e->mode(a, e->block, e->iv);
}

static inline void testcipher_from_block_wrong_iv_len(void *env, TestingT *t) {
    (void)env;
    testing_t_skip_v(t, "see Issue 68377");
}

static inline void testcipher_from_block_stream(void *env, TestingT *t) {
    TestcipherFromBlockEnv e = *(const TestcipherFromBlockEnv *)env;
    TESTCIPHER_ARENA;
    TesthashRand rng = testhash_new_rand(t);
    e.iv = testhash_bytes(a, cipher_block_block_size(e.block));
    testhash_read(&rng, e.iv);
    testcipher_stream(t, testcipher_from_block_make, &e);
    arena_free(&ar);
}

/* Go's TestStreamFromBlock. */
static inline void testcipher_stream_from_block(TestingT *t, CipherBlock block,
                                                TestcipherStreamMode mode) {
    TestcipherFromBlockEnv e = {block, mode, {0}};
    testing_t_run(t, BURROW_S("WrongIVLen"),
                  BURROW_FN(TestingTFunc, testcipher_from_block_wrong_iv_len, &e));
    testing_t_run(t, BURROW_S("BlockModeStream"),
                  BURROW_FN(TestingTFunc, testcipher_from_block_stream, &e));
}

/* --------------------------------------------------------------------- AEAD */

typedef CipherAEAD (*TestcipherMakeAEAD)(Alloc *a, void *env, Error *err);

static const Int testcipher_lengths[] = {0, 156, 8192, 8193, 8208};
#define TESTCIPHER_NLENGTHS                                                            \
    ((Int)(sizeof testcipher_lengths / sizeof testcipher_lengths[0]))

typedef struct TestcipherAEADEnv {
    CipherAEAD aead;
    Int pt_len, ad_len;
    const char *boundary;
    Slice plaintext, ad, nonce;
} TestcipherAEADEnv;

/* Go's sealMsg. */
static inline Slice testcipher_seal(TestingT *t, Alloc *a, CipherAEAD aead, Slice dst,
                                    Slice nonce, Slice plaintext, Slice ad) {
    testing_t_helper(t);
    Int initial = dst.len;
    dst = cipher_aead_seal(aead, a, dst, nonce, plaintext, ad);
    Int n = dst.len - initial;
    if (n > plaintext.len + cipher_aead_overhead(aead))
        testing_t_errorf_v(t,
                           "length of ciphertext from Seal exceeds length of plaintext "
                           "by more than Overhead(); got %d, want <=%d",
                           n, plaintext.len + cipher_aead_overhead(aead));
    return dst;
}

/* Go's openWithoutError. */
static inline Slice testcipher_open(TestingT *t, Alloc *a, CipherAEAD aead, Slice dst,
                                    Slice nonce, Slice ciphertext, Slice ad) {
    testing_t_helper(t);
    Error err = BURROW_NO_ERROR;
    Slice out = cipher_aead_open(aead, a, dst, nonce, ciphertext, ad, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t,
                           "Open returned error on properly formed ciphertext; got "
                           "\"%v\", want \"nil\"",
                           err);
    return out;
}

/* Go's isDeterministic. */
static inline bool testcipher_is_deterministic(Alloc *a, CipherAEAD aead) {
    Slice nonce = testhash_bytes(a, cipher_aead_nonce_size(aead));
    Slice ad = slice_from_str(a, BURROW_S("additional data"));
    Slice pt = slice_from_str(a, BURROW_S("plaintext"));
    Slice c1 = cipher_aead_seal(aead, a, slice_nil(TYPE_BYTE), nonce, pt, ad);
    Slice c2 = cipher_aead_seal(aead, a, slice_nil(TYPE_BYTE), nonce, pt, ad);
    return testhash_equal(c1, c2);
}

static inline Slice testcipher_nonce(Alloc *a, TesthashRand *rng, CipherAEAD aead) {
    Slice nonce = testhash_bytes(a, cipher_aead_nonce_size(aead));
    testhash_read(rng, nonce);
    return nonce;
}

static inline Str testcipher_lengths_name(Alloc *a, Int pt, Int ad) {
    return fmt_sprintf_v(a, "Plaintext-Length=%d,AddData-Length=%d", pt, ad);
}

/* Runs fn once for every pair of lengths, as a subtest named for them, skipping
 * a plaintext length of min_pt or less and an additional data length of
 * min_ad or less. Go writes the two loops out every time. */
static inline void testcipher_aead_each(TestingT *t, const TestcipherAEADEnv *e,
                                        Int min_pt, Int min_ad,
                                        void (*fn)(void *, TestingT *)) {
    TESTCIPHER_ARENA;
    for (Int i = 0; i < TESTCIPHER_NLENGTHS; i++) {
        if (testcipher_lengths[i] <= min_pt)
            continue;
        for (Int j = 0; j < TESTCIPHER_NLENGTHS; j++) {
            if (testcipher_lengths[j] <= min_ad)
                continue;
            TestcipherAEADEnv one = *e;
            one.pt_len = testcipher_lengths[i];
            one.ad_len = testcipher_lengths[j];
            testing_t_run(t, testcipher_lengths_name(a, one.pt_len, one.ad_len),
                          BURROW_FN(TestingTFunc, fn, &one));
        }
    }
    arena_free(&ar);
}

static inline void testcipher_aead_roundtrip_one(void *env, TestingT *t) {
    const TestcipherAEADEnv *e = env;
    TESTCIPHER_ARENA;
    TesthashRand rng = testhash_new_rand(t);
    Slice nonce = testcipher_nonce(a, &rng, e->aead);
    /* Go's names, which have the two lengths the other way round. */
    Slice before = testhash_bytes(a, e->ad_len), ad = testhash_bytes(a, e->pt_len);
    testhash_read(&rng, before);
    testhash_read(&rng, ad);
    Slice ct = testcipher_seal(t, a, e->aead, slice_nil(TYPE_BYTE), nonce, before, ad);
    Slice after = testcipher_open(t, a, e->aead, slice_nil(TYPE_BYTE), nonce, ct, ad);
    if (!testhash_equal(after, before))
        testing_t_errorf_v(t,
                           "plaintext is different after a seal/open cycle; got %s, "
                           "want %s",
                           testcipher_hex(a, after), testcipher_hex(a, before));
    arena_free(&ar);
}

static inline void testcipher_aead_roundtrip(void *env, TestingT *t) {
    testcipher_aead_each(t, env, -1, -1, testcipher_aead_roundtrip_one);
}

static inline Slice testcipher_guarded(TestingT *t, const char *boundary, Slice b,
                                       TestcipherGuard *g) {
    Slice start, end;
    testcipher_boundary(t, b.len, g, &start, &end);
    if (strcmp(boundary, "Start") == 0) {
        slice_copy(start, b);
        return start;
    }
    slice_copy(end, b);
    return end;
}

static inline void testcipher_aead_boundary(void *env, TestingT *t) {
    const TestcipherAEADEnv *e = env;
    TESTCIPHER_ARENA;
    TestcipherGuard g[4] = {{0}};
    Slice src = testcipher_guarded(t, e->boundary, e->plaintext, &g[0]);
    Slice ad = testcipher_guarded(t, e->boundary, e->ad, &g[1]);
    Slice dst = testcipher_guarded(
        t, e->boundary, testhash_bytes(a, e->pt_len + cipher_aead_overhead(e->aead)),
        &g[2]);
    Slice ct = testcipher_seal(t, a, e->aead, slice_sub(dst, 0, 0), e->nonce, src, ad);
    Slice out = testcipher_guarded(t, e->boundary, e->plaintext, &g[3]);
    Slice after =
        testcipher_open(t, a, e->aead, slice_sub(out, 0, 0), e->nonce, ct, ad);
    if (!testhash_equal(after, e->plaintext))
        testing_t_errorf_v(t,
                           "plaintext is different after a seal/open cycle; got %s, "
                           "want %s",
                           testcipher_hex(a, after), testcipher_hex(a, e->plaintext));
    for (int i = 0; i < 4; i++)
        testcipher_release(&g[i]);
    arena_free(&ar);
}

static inline void testcipher_aead_out_of_bounds_one(void *env, TestingT *t) {
    TestcipherAEADEnv e = *(const TestcipherAEADEnv *)env;
    TESTCIPHER_ARENA;
    TesthashRand rng = testhash_new_rand(t);
    e.nonce = testcipher_nonce(a, &rng, e.aead);
    e.plaintext = testhash_bytes(a, e.pt_len);
    e.ad = testhash_bytes(a, 16);
    testhash_read(&rng, e.plaintext);
    testhash_read(&rng, e.ad);
    static const char *const boundaries[] = {"Start", "End"};
    for (int i = 0; i < 2; i++) {
        e.boundary = boundaries[i];
        testing_t_run(t, fmt_sprintf_v(a, "Boundary=%s", str_from_cstr(boundaries[i])),
                      BURROW_FN(TestingTFunc, testcipher_aead_boundary, &e));
    }
    arena_free(&ar);
}

static inline void testcipher_aead_out_of_bounds(void *env, TestingT *t) {
    static const Int boundary_lengths[] = {0,  1,   2,   3,   4,   5,   6,   7,   8,
                                           9,  10,  11,  12,  13,  14,  15,  16,  17,
                                           18, 127, 128, 129, 130, 131, 156, 8193};
    TESTCIPHER_ARENA;
    TestcipherAEADEnv e = *(const TestcipherAEADEnv *)env;
    for (size_t i = 0; i < sizeof boundary_lengths / sizeof boundary_lengths[0]; i++) {
        e.pt_len = boundary_lengths[i];
        testing_t_run(t, fmt_sprintf_v(a, "Plaintext-Length=%d", e.pt_len),
                      BURROW_FN(TestingTFunc, testcipher_aead_out_of_bounds_one, &e));
    }
    arena_free(&ar);
}

static inline void testcipher_aead_input_seal(void *env, TestingT *t) {
    const TestcipherAEADEnv *e = env;
    TESTCIPHER_ARENA;
    TesthashRand rng = testhash_new_rand(t);
    Slice nonce = testcipher_nonce(a, &rng, e->aead);
    Slice src = testhash_bytes(a, e->pt_len), before = testhash_bytes(a, e->pt_len);
    testhash_read(&rng, src);
    slice_copy(before, src);
    Slice ad = testhash_bytes(a, e->ad_len);
    testhash_read(&rng, ad);
    (void)testcipher_seal(t, a, e->aead, slice_nil(TYPE_BYTE), nonce, src, ad);
    if (!testhash_equal(src, before))
        testing_t_errorf_v(t, "Seal modified src; got %s, want %s",
                           testcipher_hex(a, src), testcipher_hex(a, before));
    arena_free(&ar);
}

static inline void testcipher_aead_input_open(void *env, TestingT *t) {
    const TestcipherAEADEnv *e = env;
    TESTCIPHER_ARENA;
    TesthashRand rng = testhash_new_rand(t);
    Slice nonce = testcipher_nonce(a, &rng, e->aead);
    Slice pt = testhash_bytes(a, e->pt_len), ad = testhash_bytes(a, e->ad_len);
    testhash_read(&rng, pt);
    testhash_read(&rng, ad);
    Slice ct = testcipher_seal(t, a, e->aead, slice_nil(TYPE_BYTE), nonce, pt, ad);
    Slice before = testhash_clone(a, ct);
    (void)testcipher_open(t, a, e->aead, slice_nil(TYPE_BYTE), nonce, ct, ad);
    if (!testhash_equal(ct, before))
        testing_t_errorf_v(t, "Open modified src; got %s, want %s",
                           testcipher_hex(a, ct), testcipher_hex(a, before));
    arena_free(&ar);
}

static inline void testcipher_aead_input_one(void *env, TestingT *t) {
    testing_t_run(t, BURROW_S("Seal"),
                  BURROW_FN(TestingTFunc, testcipher_aead_input_seal, env));
    testing_t_run(t, BURROW_S("Open"),
                  BURROW_FN(TestingTFunc, testcipher_aead_input_open, env));
}

static inline void testcipher_aead_input_not_modified(void *env, TestingT *t) {
    testcipher_aead_each(t, env, -1, -1, testcipher_aead_input_one);
}

static inline void testcipher_seal_op(TestcipherOp *op) {
    const TestcipherAEADEnv *e = op->env;
    (void)cipher_aead_seal(e->aead, op->a, op->dst, op->nonce, op->src, op->ad);
}

static inline void testcipher_open_op(TestcipherOp *op) {
    const TestcipherAEADEnv *e = op->env;
    Error err = BURROW_NO_ERROR;
    (void)cipher_aead_open(e->aead, op->a, op->dst, op->nonce, op->src, op->ad, &err);
}

static inline void testcipher_aead_overlap_seal(void *env, TestingT *t) {
    const TestcipherAEADEnv *e = env;
    TESTCIPHER_ARENA;
    TesthashRand rng = testhash_new_rand(t);
    Int pt_len = e->pt_len;
    TestcipherOp op = {testcipher_seal_op, e, a, {0}, {0}, {0}, {0}};
    op.nonce = testcipher_nonce(a, &rng, e->aead);
    Int ct_len = pt_len + cipher_aead_overhead(e->aead);
    Slice buff = testhash_bytes(a, pt_len + ct_len);
    testhash_read(&rng, buff);
    op.ad = testhash_bytes(a, e->ad_len);
    testhash_read(&rng, op.ad);

    op.src = slice_sub(buff, 0, pt_len);
    op.dst = slice_sub(buff, 1, 1);
    testcipher_must_panic(t, "invalid buffer overlap", &op);

    op.src = slice_sub(buff, 0, pt_len);
    op.dst = slice_sub(buff, pt_len - 1, pt_len - 1);
    testcipher_must_panic(t, "invalid buffer overlap", &op);
    arena_free(&ar);
}

static inline void testcipher_aead_overlap_open(void *env, TestingT *t) {
    const TestcipherAEADEnv *e = env;
    TESTCIPHER_ARENA;
    TesthashRand rng = testhash_new_rand(t);
    Int pt_len = e->pt_len;
    TestcipherOp op = {testcipher_open_op, e, a, {0}, {0}, {0}, {0}};
    op.nonce = testcipher_nonce(a, &rng, e->aead);
    Slice pt = testhash_bytes(a, pt_len);
    testhash_read(&rng, pt);
    op.ad = testhash_bytes(a, e->ad_len);
    testhash_read(&rng, op.ad);
    Slice valid =
        testcipher_seal(t, a, e->aead, slice_nil(TYPE_BYTE), op.nonce, pt, op.ad);

    Slice buff = testhash_bytes(a, pt_len + valid.len);
    op.src = slice_sub(buff, 0, valid.len);
    slice_copy(op.src, valid);
    op.dst = slice_sub(buff, 1, 1);
    testcipher_must_panic(t, "invalid buffer overlap", &op);

    op.src = slice_sub(buff, 0, valid.len);
    slice_copy(op.src, valid);
    Int before_tag = valid.len - cipher_aead_overhead(e->aead);
    op.dst = slice_sub(buff, before_tag - 1, before_tag - 1);
    testcipher_must_panic(t, "invalid buffer overlap", &op);
    arena_free(&ar);
}

static inline void testcipher_aead_overlap_one(void *env, TestingT *t) {
    testing_t_run(t, BURROW_S("Seal"),
                  BURROW_FN(TestingTFunc, testcipher_aead_overlap_seal, env));
    testing_t_run(t, BURROW_S("Open"),
                  BURROW_FN(TestingTFunc, testcipher_aead_overlap_open, env));
}

static inline void testcipher_aead_buffer_overlap(void *env, TestingT *t) {
    /* A plaintext of 1 byte or less leaves no room for an inexact overlap. */
    testcipher_aead_each(t, env, 1, -1, testcipher_aead_overlap_one);
}

static inline void testcipher_aead_append_seal(void *env, TestingT *t) {
    const TestcipherAEADEnv *e = env;
    TESTCIPHER_ARENA;
    TesthashRand rng = testhash_new_rand(t);
    Slice nonce = testcipher_nonce(a, &rng, e->aead);
    Slice short_buff = slice_from_str(a, BURROW_S("a"));
    Slice long_buff = testhash_bytes(a, 512);
    testhash_read(&rng, long_buff);
    Slice prefixes[] = {short_buff, long_buff};
    for (int i = 0; i < 2; i++) {
        Slice prefix = prefixes[i];
        Slice pt = testhash_bytes(a, e->pt_len), ad = testhash_bytes(a, e->ad_len);
        testhash_read(&rng, pt);
        testhash_read(&rng, ad);
        Slice out = testcipher_seal(t, a, e->aead, prefix, nonce, pt, ad);
        Slice head = slice_sub(out, 0, prefix.len);
        if (!testhash_equal(head, prefix))
            testing_t_errorf_v(t,
                               "Seal alters dst instead of appending; got %s, want %s",
                               testcipher_hex(a, head), testcipher_hex(a, prefix));
        if (testcipher_is_deterministic(a, e->aead)) {
            Slice ct = slice_sub(out, prefix.len, out.len);
            Slice expected =
                testcipher_seal(t, a, e->aead, slice_nil(TYPE_BYTE), nonce, pt, ad);
            if (!testhash_equal(ct, expected))
                testing_t_errorf_v(
                    t,
                    "Seal behavior affected by pre-existing data in dst; "
                    "got %s, want %s",
                    testcipher_hex(a, ct), testcipher_hex(a, expected));
        }
    }
    arena_free(&ar);
}

static inline void testcipher_aead_append_open(void *env, TestingT *t) {
    const TestcipherAEADEnv *e = env;
    TESTCIPHER_ARENA;
    TesthashRand rng = testhash_new_rand(t);
    Slice nonce = testcipher_nonce(a, &rng, e->aead);
    Slice short_buff = slice_from_str(a, BURROW_S("a"));
    Slice long_buff = testhash_bytes(a, 512);
    testhash_read(&rng, long_buff);
    Slice prefixes[] = {short_buff, long_buff};
    for (int i = 0; i < 2; i++) {
        Slice prefix = prefixes[i];
        /* Go's names again, the other way round. */
        Slice before = testhash_bytes(a, e->ad_len), ad = testhash_bytes(a, e->pt_len);
        testhash_read(&rng, before);
        testhash_read(&rng, ad);
        Slice ct =
            testcipher_seal(t, a, e->aead, slice_nil(TYPE_BYTE), nonce, before, ad);
        Slice out = testcipher_open(t, a, e->aead, prefix, nonce, ct, ad);
        Slice head = slice_sub(out, 0, prefix.len);
        if (!testhash_equal(head, prefix))
            testing_t_errorf_v(t,
                               "Open alters dst instead of appending; got %s, want %s",
                               testcipher_hex(a, head), testcipher_hex(a, prefix));
        Slice after = slice_sub(out, prefix.len, out.len);
        if (!testhash_equal(after, before))
            testing_t_errorf_v(
                t,
                "Open behavior affected by pre-existing data in dst; got "
                "%s, want %s",
                testcipher_hex(a, after), testcipher_hex(a, before));
    }
    arena_free(&ar);
}

static inline void testcipher_aead_append_one(void *env, TestingT *t) {
    testing_t_run(t, BURROW_S("Seal"),
                  BURROW_FN(TestingTFunc, testcipher_aead_append_seal, env));
    testing_t_run(t, BURROW_S("Open"),
                  BURROW_FN(TestingTFunc, testcipher_aead_append_open, env));
}

static inline void testcipher_aead_append_dst(void *env, TestingT *t) {
    testcipher_aead_each(t, env, -1, -1, testcipher_aead_append_one);
}

/* The three checks that change one thing and expect Open to refuse. which is
 * 0 for the nonce, 1 for the additional data and 2 for the ciphertext. */
static inline void testcipher_aead_wrong(TestingT *t, const TestcipherAEADEnv *e,
                                         int which) {
    TESTCIPHER_ARENA;
    TesthashRand rng = testhash_new_rand(t);
    Slice nonce = testcipher_nonce(a, &rng, e->aead);
    Slice pt = testhash_bytes(a, e->pt_len), ad = testhash_bytes(a, e->ad_len);
    testhash_read(&rng, pt);
    testhash_read(&rng, ad);
    Slice ct = testcipher_seal(t, a, e->aead, slice_nil(TYPE_BYTE), nonce, pt, ad);

    Slice *alter = which == 0 ? &nonce : which == 1 ? &ad : &ct;
    *alter = testhash_clone(a, *alter);
    ((Byte *)alter->p)[alter->len - 1] += 1;

    Error err = BURROW_NO_ERROR;
    (void)cipher_aead_open(e->aead, a, slice_nil(TYPE_BYTE), nonce, ct, ad, &err);
    if (!BURROW_FAILED(err)) {
        if (which == 0)
            testing_t_errorf_v(t, "Open did not error when given different nonce than "
                                  "Sealed with");
        else if (which == 1)
            testing_t_errorf_v(t, "Open did not error when given different Additional "
                                  "Data than Sealed with");
        else
            testing_t_errorf_v(t, "Open did not error when given different ciphertext "
                                  "than was produced by Seal");
    }
    arena_free(&ar);
}

static inline void testcipher_aead_wrong_nonce_one(void *env, TestingT *t) {
    testcipher_aead_wrong(t, env, 0);
}

static inline void testcipher_aead_wrong_ad_one(void *env, TestingT *t) {
    testcipher_aead_wrong(t, env, 1);
}

static inline void testcipher_aead_wrong_ct_one(void *env, TestingT *t) {
    testcipher_aead_wrong(t, env, 2);
}

static inline void testcipher_aead_wrong_nonce(void *env, TestingT *t) {
    const TestcipherAEADEnv *e = env;
    if (cipher_aead_nonce_size(e->aead) == 0)
        testing_t_skip_v(t, "AEAD does not use a nonce");
    testcipher_aead_each(t, env, -1, -1, testcipher_aead_wrong_nonce_one);
}

static inline void testcipher_aead_wrong_add_data(void *env, TestingT *t) {
    testcipher_aead_each(t, env, -1, 0, testcipher_aead_wrong_ad_one);
}

static inline void testcipher_aead_wrong_ciphertext(void *env, TestingT *t) {
    testcipher_aead_each(t, env, -1, -1, testcipher_aead_wrong_ct_one);
}

/* Go's TestAEAD. make is called once, with env, and the AEAD it makes lives
 * until this returns. */
static inline void testcipher_aead(TestingT *t, TestcipherMakeAEAD make, void *env) {
    TESTCIPHER_ARENA;
    Error err = BURROW_NO_ERROR;
    TestcipherAEADEnv e = {0};
    e.aead = make(a, env, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%v", err);

    testing_t_run(t, BURROW_S("Roundtrip"),
                  BURROW_FN(TestingTFunc, testcipher_aead_roundtrip, &e));
    testing_t_run(t, BURROW_S("OutOfBounds"),
                  BURROW_FN(TestingTFunc, testcipher_aead_out_of_bounds, &e));
    testing_t_run(t, BURROW_S("InputNotModified"),
                  BURROW_FN(TestingTFunc, testcipher_aead_input_not_modified, &e));
    testing_t_run(t, BURROW_S("BufferOverlap"),
                  BURROW_FN(TestingTFunc, testcipher_aead_buffer_overlap, &e));
    testing_t_run(t, BURROW_S("AppendDst"),
                  BURROW_FN(TestingTFunc, testcipher_aead_append_dst, &e));
    testing_t_run(t, BURROW_S("WrongNonce"),
                  BURROW_FN(TestingTFunc, testcipher_aead_wrong_nonce, &e));
    testing_t_run(t, BURROW_S("WrongAddData"),
                  BURROW_FN(TestingTFunc, testcipher_aead_wrong_add_data, &e));
    testing_t_run(t, BURROW_S("WrongCiphertext"),
                  BURROW_FN(TestingTFunc, testcipher_aead_wrong_ciphertext, &e));
    arena_free(&ar);
}

/* ------------------------------------------------------ all implementations */

static inline void testcipher_impl_unsupported(void *env, TestingT *t) {
    (void)env;
    testing_t_skip_v(t, "implementation not supported");
}

static inline void testcipher_impl_reset(void *env) {
    (void)env;
    burrow__aes_set_portable(false);
}

/* Go's TestAllImplementations for "aes" and "gcm", which register the same
 * names: the instructions when this processor has them, and then "Base", the
 * portable code. */
static inline void testcipher_all_implementations(TestingT *t,
                                                  void (*f)(void *env, TestingT *t),
                                                  void *env) {
    testing_t_cleanup(t, BURROW_FN(Func, testcipher_impl_reset, NULL));

    bool available = false;
    const char *name = burrow__aes_hardware(&available);
    if (name != NULL) {
        burrow__aes_set_portable(false);
        if (available)
            testing_t_run(t, str_from_cstr(name), BURROW_FN(TestingTFunc, f, env));
        else
            testing_t_run(t, str_from_cstr(name),
                          BURROW_FN(TestingTFunc, testcipher_impl_unsupported, NULL));
    }

    burrow__aes_set_portable(true);
    testing_t_run(t, BURROW_S("Base"), BURROW_FN(TestingTFunc, f, env));
    burrow__aes_set_portable(false);
}

#endif /* BURROW_TESTS_TESTCIPHER_H */
