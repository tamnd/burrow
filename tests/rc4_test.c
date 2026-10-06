/* Derived from Go's src/crypto/rc4/rc4_test.go.
 * Go source: go1.27.1.
 *
 * TestKeySizeError and TestReset are burrow's.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"
#include "testcipher.h"

#include "burrow/burrow.h"
#include "burrow/bytes.h"
#include "burrow/crypto/rc4.h"
#include "burrow/encoding/hex.h"

#include <stdint.h>
#include <string.h>

/* The key and the key stream it starts with, as hex. */
typedef struct Rc4Test {
    const char *key, *keystream;
} Rc4Test;

static const Rc4Test golden[] = {
    /* Test vectors from the original cypherpunk posting of ARC4:
     *   https://groups.google.com/group/sci.crypt/msg/10a300c9d21afca0?pli=1 */
    {"0123456789abcdef", "7494c2e7104b0879"},
    {"0000000000000000", "de188941a3375d3a"},
    {"ef012345", "d6a141a7ec3c38dfbd61"},

    /* Test vectors from the Wikipedia page: https://en.wikipedia.org/wiki/RC4 */
    {"4b6579", "eb9f7781b734ca72a719"},
    {"57696b69", "6044db6d41b7"},
    {"0000000000000000",
     "de188941a3375d3a8a061e67576e926dc71a7fa3f0cceb97452b4d3227965f9e"
     "a8cc75076d9fb9c5417aa5cb30fc22198b34982dbb629ec04b4f8b05a0710850"
     "92a0c3584a48e4a30a397b8acd1d009ec87d6811f22cf49ca3e59354b9451535"
     "a2187a86426cca7d5e823eba004412671257b8d860ae4cbd4c4906bbc535efe1"
     "587f08db33955cdbcbad9b10f53fc4e52c591565518487fe084d0e3f03debcc9"
     "da1ce90d085c2d8a19d8373086163692142bd8fc5d7a73496a8e59ee7ecf6b94"
     "0663f4a6bee65bd2c85c46986c1bef3490d37b38da85d32e9739cb234a2be740"},
};

#define NGOLDEN ((Int)(sizeof golden / sizeof golden[0]))

static Slice unhex(Alloc *a, const char *s) {
    Error err = BURROW_NO_ERROR;
    Slice b = hex_decode_string(a, str_from_cstr(s), &err);
    if (BURROW_FAILED(err))
        panic_str(BURROW_S("bad hex in a test table"));
    return b;
}

static Rc4Cipher *new_cipher(Alloc *a, const char *key) {
    Error err = BURROW_NO_ERROR;
    Rc4Cipher *c = rc4_new_cipher(a, unhex(a, key), &err);
    if (BURROW_FAILED(err))
        panic(BURROW_ANY(TYPE_ERROR, &err));
    return c;
}

static void test_encrypt(TestingT *t, Str desc, Rc4Cipher *c, Slice src, Slice expect) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Slice dst = slice_make(arena_allocator(&ar), TYPE_BYTE, src.len, src.len);
    rc4_cipher_xor_key_stream(c, dst, src);
    const Byte *d = dst.p, *e = expect.p;
    for (Int i = 0; i < dst.len; i++) {
        if (d[i] != e[i])
            testing_t_fatalf_v(t, "%s: mismatch at byte %d:\nhave %x\nwant %x", desc, i,
                               dst, expect);
    }
    arena_free(&ar);
}

static void TestGolden(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int gi = 0; gi < NGOLDEN; gi++) {
        const Rc4Test *g = &golden[gi];
        Slice keystream = unhex(a, g->keystream);
        Int n_ks = keystream.len;
        Slice data = slice_make(a, TYPE_BYTE, n_ks, n_ks);
        for (Int i = 0; i < n_ks; i++)
            ((Byte *)data.p)[i] = (Byte)i;

        Slice expect = slice_make(a, TYPE_BYTE, n_ks, n_ks);
        for (Int i = 0; i < n_ks; i++)
            ((Byte *)expect.p)[i] = (Byte)((Byte)i ^ ((const Byte *)keystream.p)[i]);

        for (Int size = 1; size <= n_ks; size++) {
            Error err = BURROW_NO_ERROR;
            Rc4Cipher *c = rc4_new_cipher(a, unhex(a, g->key), &err);
            if (BURROW_FAILED(err))
                testing_t_fatalf_v(t, "#%d: NewCipher: %v", gi, err);

            Int off = 0;
            while (off < n_ks) {
                Int n = n_ks - off;
                if (n > size)
                    n = size;
                Str desc = fmt_sprintf_v(a, "#%d@[%d:%d]", gi, off, off + n);
                test_encrypt(t, desc, c, slice_sub(data, off, off + n),
                             slice_sub(expect, off, off + n));
                off += n;
            }
        }
    }
    arena_free(&ar);
}

static void TestBlock(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Rc4Cipher *c1a = new_cipher(a, golden[0].key);
    Rc4Cipher *c1b = new_cipher(a, golden[1].key);
    Slice data1 = slice_make(a, TYPE_BYTE, 1 << 20, 1 << 20);
    for (Int i = 0; i < data1.len; i++) {
        rc4_cipher_xor_key_stream(c1a, slice_sub(data1, i, i + 1),
                                  slice_sub(data1, i, i + 1));
        rc4_cipher_xor_key_stream(c1b, slice_sub(data1, i, i + 1),
                                  slice_sub(data1, i, i + 1));
    }

    Rc4Cipher *c2a = new_cipher(a, golden[0].key);
    Rc4Cipher *c2b = new_cipher(a, golden[1].key);
    Slice data2 = slice_make(a, TYPE_BYTE, 1 << 20, 1 << 20);
    rc4_cipher_xor_key_stream(c2a, data2, data2);
    rc4_cipher_xor_key_stream(c2b, data2, data2);

    if (!bytes_equal(data1, data2))
        testing_t_fatalf_v(t, "bad block");
    arena_free(&ar);
}

static CipherStream make_stream(Alloc *a, void *env) {
    (void)env;
    return rc4_cipher_as_cipher_stream(new_cipher(a, golden[0].key));
}

static void TestRC4Stream(TestingT *t) {
    testcipher_stream(t, make_stream, NULL);
}

/* Not Go's: the key sizes NewCipher refuses, and its error for them. */
static void TestKeySizeError(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    static const Byte key[300] = {0};
    static const Int bad[] = {0, 257, 300};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        Slice k = slice_from((void *)(uintptr_t)key, bad[i], bad[i], TYPE_BYTE);
        Error err = BURROW_NO_ERROR;
        Rc4Cipher *c = rc4_new_cipher(a, k, &err);
        if (!BURROW_FAILED(err) || c != NULL) {
            testing_t_errorf_v(t, "NewCipher(%d bytes) succeeded", bad[i]);
            continue;
        }
        Str want = fmt_sprintf_v(a, "crypto/rc4: invalid key size %d", bad[i]);
        if (!str_eq(error_text(err), want))
            testing_t_errorf_v(t, "NewCipher(%d bytes) = %q, want %q", bad[i],
                               error_text(err), want);
        const Rc4KeySizeError *ks = errors_as(err, TYPE_RC4_KEY_SIZE_ERROR);
        if (ks == NULL || *ks != bad[i])
            testing_t_errorf_v(
                t, "NewCipher(%d bytes): errors.As does not give the size", bad[i]);
    }
    for (Int n = 1; n <= 256; n++) {
        Error err = BURROW_NO_ERROR;
        Slice k = slice_from((void *)(uintptr_t)key, n, n, TYPE_BYTE);
        if (rc4_new_cipher(a, k, &err) == NULL || BURROW_FAILED(err))
            testing_t_errorf_v(t, "NewCipher(%d bytes) = %v", n, err);
    }
    arena_free(&ar);
}

/* Not Go's: Reset leaves nothing of the key behind in the state. */
static void TestReset(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Rc4Cipher *c = new_cipher(a, golden[0].key);
    Byte buf[16] = {0};
    Slice b = slice_from(buf, 16, 16, TYPE_BYTE);
    rc4_cipher_xor_key_stream(c, b, b);
    rc4_cipher_reset(c);
    for (int i = 0; i < 256; i++) {
        if (c->s[i] != 0) {
            testing_t_errorf_v(t, "s[%d] = %d after Reset", (Int)i, (Int)c->s[i]);
            break;
        }
    }
    if (c->i != 0 || c->j != 0)
        testing_t_errorf_v(t, "i, j = %d, %d after Reset", (Int)c->i, (Int)c->j);
    arena_free(&ar);
}

static void benchmark(TestingB *b, Int size) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice buf = slice_make(a, TYPE_BYTE, size, size);
    Rc4Cipher *c = new_cipher(a, golden[0].key);
    testing_b_set_bytes(b, size);

    for (Int i = 0; i < testing_b_n(b); i++)
        rc4_cipher_xor_key_stream(c, buf, buf);
    arena_free(&ar);
}

static void BenchmarkRC4_128(TestingB *b) {
    benchmark(b, 128);
}

static void BenchmarkRC4_1K(TestingB *b) {
    benchmark(b, 1024);
}

static void BenchmarkRC4_8K(TestingB *b) {
    benchmark(b, 8096);
}

#define TESTS(X)                                                                       \
    X(TestGolden)                                                                      \
    X(TestBlock)                                                                       \
    X(TestRC4Stream)                                                                   \
    X(TestKeySizeError)                                                                \
    X(TestReset)                                                                       \
    X(BenchmarkRC4_128)                                                                \
    X(BenchmarkRC4_1K)                                                                 \
    X(BenchmarkRC4_8K)

TESTING_MAIN(TESTS)
