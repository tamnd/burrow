/* Derived from Go's src/crypto/internal/fips140test/cmac_test.go and
 * cmac_wycheproof_test.go.
 * Go source: go1.27.1.
 *
 * CMAC is internal in Go and here, so this reaches it through aes_internal.h.
 * The Wycheproof vectors Go fetches as a module are in cmac_test_gen.h, which
 * tools/gen-cmac-tests.sh writes. Go runs each one as a parallel subtest; here
 * the subtests run one after another.
 *
 * Copyright 2024 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "../src/crypto/aes_internal.h"

#include "burrow/burrow.h"
#include "burrow/bytes.h"
#include "burrow/crypto/aes.h"
#include "burrow/encoding/hex.h"
#include "burrow/mem/arena.h"
#include "burrow/strings.h"

#include "cmac_test_gen.h"

#include <stdint.h>
#include <stdio.h>

static Slice tag_bytes(Byte tag[AES_BLOCK_SIZE]) {
    return slice_from(tag, AES_BLOCK_SIZE, AES_BLOCK_SIZE, TYPE_BYTE);
}

/* fips_test.go's decodeHex, which takes the spaces out first. It reports the
 * error and gives false, and the caller frees and stops. */
static bool decode_hex(TestingT *t, Alloc *a, const char *s, Slice *out) {
    Error err = BURROW_NO_ERROR;
    Str h = strings_replace_all(a, str_from_cstr(s), BURROW_S(" "), BURROW_S(""));
    *out = hex_decode_string(a, h, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "%v", err);
        return false;
    }
    if (out->elem == NULL)
        *out = slice_nil(TYPE_BYTE);
    return true;
}

/* aes.New, and the Block of the FIPS module behind it. */
static bool new_aes_block(TestingT *t, Alloc *a, Slice key, const AesBlock **out) {
    Error err = BURROW_NO_ERROR;
    CipherBlock b = aes_new_cipher(a, key, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "aes.New: %v", err);
        return false;
    }
    *out = burrow__aes_block_of(b);
    if (*out == NULL) {
        /* Not in Go: aes.New there is always the FIPS module's block. */
        testing_t_errorf_v(t, "aes.New: not an AES block");
        return false;
    }
    return true;
}

typedef struct CmacTest {
    const char *in, *out;
} CmacTest;

static void TestCMAC(TestingT *t) {
    /* https://csrc.nist.gov/CSRC/media/Projects/Cryptographic-Standards-and-Guidelines/documents/examples/AES_CMAC.pdf */
    const char *key = "2B7E1516 28AED2A6 ABF71588 09CF4F3C";
    static const CmacTest tests[] = {
        {
            "",
            "BB1D6929 E9593728 7FA37D12 9B756746",
        },
        {
            "6BC1BEE2 2E409F96 E93D7E11 7393172A",
            "070A16B4 6B4D4144 F79BDD9D D04A287C",
        },
        {
            "6BC1BEE2 2E409F96 E93D7E11 7393172A AE2D8A57",
            "7D85449E A6EA19C8 23A7BF78 837DFADE",
        },
    };

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice k;
    const AesBlock *b = NULL;
    if (!decode_hex(t, a, key, &k) || !new_aes_block(t, a, k, &b)) {
        arena_free(&ar);
        testing_t_fail_now(t);
    }
    Cmac c;
    burrow__cmac_init(&c, b);
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Slice in, out;
        if (!decode_hex(t, a, tests[i].in, &in) ||
            !decode_hex(t, a, tests[i].out, &out)) {
            arena_free(&ar);
            testing_t_fail_now(t);
        }
        Byte got[AES_BLOCK_SIZE];
        burrow__cmac_mac(&c, got, in);
        Slice g = tag_bytes(got);
        int n = (int)i;
        if (!bytes_equal(g, out))
            testing_t_errorf_v(t, "test %d: got %x, want %x", n, g, out);
    }
    arena_free(&ar);
}

static Slice unhex(Alloc *a, const char *s) {
    Slice b = hex_decode_string(a, str_from_cstr(s), NULL);
    if (b.elem == NULL)
        b = slice_nil(TYPE_BYTE);
    return b;
}

static void cmac_wycheproof(void *env, TestingT *t) {
    const GenWycheproof *tv = env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    Slice key = unhex(a, tv->key);
    Slice msg = unhex(a, tv->msg);
    Slice expected_tag = unhex(a, tv->tag);
    bool want_pass = tv->want_pass;

    const AesBlock *b = NULL;
    if (!new_aes_block(t, a, key, &b)) {
        arena_free(&ar);
        testing_t_fail_now(t);
    }
    Cmac c;
    burrow__cmac_init(&c, b);
    Byte tag[AES_BLOCK_SIZE];
    burrow__cmac_mac(&c, tag, msg);
    Slice got = tag_bytes(tag);

    if (bytes_equal(got, expected_tag)) {
        if (!want_pass)
            testing_t_errorf_v(t, "expected failure but tag matched");
    } else {
        if (want_pass)
            testing_t_errorf_v(t, "tag mismatch: got %x, want %x", got, expected_tag);
    }
    arena_free(&ar);
}

static void TestCMACWycheproof(TestingT *t) {
    for (size_t i = 0; i < sizeof gen_wycheproof / sizeof gen_wycheproof[0]; i++) {
        const GenWycheproof *tv = &gen_wycheproof[i];
        char name[256];
        if (tv->comment[0] != '\0')
            snprintf(name, sizeof name, "%s #%d %s", tv->file, tv->tc_id, tv->comment);
        else
            snprintf(name, sizeof name, "%s #%d", tv->file, tv->tc_id);
        testing_t_run(t, str_from_cstr(name),
                      BURROW_FN(TestingTFunc, cmac_wycheproof, (void *)(uintptr_t)tv));
    }
}

#define TESTS(X)                                                                       \
    X(TestCMAC)                                                                        \
    X(TestCMACWycheproof)

TESTING_MAIN(TESTS)
