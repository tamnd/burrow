/* Derived from Go's src/crypto/tls/ech_test.go.
 * Go source: go1.27.1.
 *
 * Copyright 2024 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "../src/crypto/tls_internal.h"
#include "burrow/burrow.h"
#include "burrow/crypto/hpke.h"
#include "burrow/crypto/tls.h"
#include "burrow/encoding/hex.h"

#include <stdint.h>
#include <string.h>

#define NELEM(x) (sizeof(x) / sizeof((x)[0]))

static Slice unhex(Alloc *a, const char *s, Error *err) {
    return hex_decode_string(a, str_from_cstr(s), err);
}

typedef struct DecodeECHConfigListsCase {
    const char *list;
    Int num_configs;
} DecodeECHConfigListsCase;

static const DecodeECHConfigListsCase decode_ech_config_lists_cases[] = {
    {"0045fe0d0041590020002092a01233db2218518ccbbbbc24df20686af417b37388de6460"
     "e94011974777090004000100010012636c6f7564666c6172652d6563682e636f6d0000",
     1},
    {"0105badd00050504030201fe0d0066000010004104e62b69e2bf659f97be2f1e0d948a4c"
     "d5976bb7a91e0d46fbdda9a91e9ddcba5a01e7d697a80a18f9c3c4a31e56e27c8348db16"
     "1a1cf51d7ef1942d4bcf7222c1000c000100010001000200010003400e7075626c69632e"
     "6578616d706c650000fe0d003d00002000207d661615730214aeee70533366f36a609ead"
     "65c0c208e62322346ab5bcd8de1c000411112222400e7075626c69632e6578616d706c65"
     "0000fe0d004d000020002085bd6a03277c25427b52e269e0c77a8eb524ba1eb3d2f13266"
     "2d4b0ac6cb7357000c000100010001000200010003400e7075626c69632e6578616d706c"
     "650008aaaa000474657374",
     3},
};

static void TestDecodeECHConfigLists(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < NELEM(decode_ech_config_lists_cases); i++) {
        const DecodeECHConfigListsCase *tc = &decode_ech_config_lists_cases[i];
        Error err = BURROW_NO_ERROR;
        Slice b = unhex(a, tc->list, &err);
        if (BURROW_FAILED(err)) {
            arena_free(&ar);
            testing_t_fatal_v(t, err);
        }
        Slice configs = burrow__tls_parse_ech_config_list(a, b, &err);
        if (BURROW_FAILED(err)) {
            arena_free(&ar);
            testing_t_fatal_v(t, err);
        }
        if (configs.len != tc->num_configs) {
            Int got = configs.len;
            arena_free(&ar);
            testing_t_fatalf_v(t, "unexpected number of configs parsed: got %d want %d",
                               got, tc->num_configs);
        }
    }
    arena_free(&ar);
}

/* pickECHConfig(configs), with the public key it makes freed. */
static const TlsECHConfig *pick(Alloc *a, Slice configs) {
    HpkePublicKey *pk = NULL;
    const HpkeKDF *kdf;
    const HpkeAEAD *aead;
    const TlsECHConfig *config =
        burrow__tls_pick_ech_config(a, configs, &pk, &kdf, &aead);
    if (pk != NULL)
        hpke_public_key_free(pk);
    return config;
}

static void TestSkipBadConfigs(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    Slice b =
        unhex(a,
              "00c8badd00050504030201fe0d0029006666000401020304000c00010001000100020001"
              "0003400e7075626c69632e6578616d706c650000fe0d003d000020002072e8a23b7aef67"
              "832bcc89d652e3870a60f88ca684ec65d6eace6b61f136064c000411112222400e707562"
              "6c69632e6578616d706c650000fe0d004d00002000200ce95810a81d8023f41e83679bc9"
              "2701b2acd46c75869f95c72bc61c6b12297c000c000100010001000200010003400e7075"
              "626c69632e6578616d706c650008aaaa000474657374",
              &err);
    if (BURROW_FAILED(err)) {
        arena_free(&ar);
        testing_t_fatal_v(t, err);
    }
    Slice configs = burrow__tls_parse_ech_config_list(a, b, &err);
    if (BURROW_FAILED(err)) {
        arena_free(&ar);
        testing_t_fatal_v(t, err);
    }
    const TlsECHConfig *config = pick(a, configs);
    arena_free(&ar);
    if (config != NULL)
        testing_t_fatal_v(t, "pickECHConfig picked an invalid config");
}

static void TestPickECHConfigWithInvalidAEADID(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    Slice b =
        unhex(a,
              "0045fe0d0041590020002092a01233db2218518ccbbbbc24df20686af417b37388de6460"
              "e94011974777090004000100010012636c6f7564666c6172652d6563682e636f6d0000",
              &err);
    if (BURROW_FAILED(err)) {
        arena_free(&ar);
        testing_t_fatal_v(t, err);
    }
    static const Byte old[4] = {0x00, 0x01, 0x00, 0x01};
    static const Byte repl[4] = {0x00, 0x01, 0xFF, 0xFF};
    Slice buf = bytes_replace(a, b, slice_from((Byte *)(uintptr_t)old, 4, 4, TYPE_BYTE),
                              slice_from((Byte *)(uintptr_t)repl, 4, 4, TYPE_BYTE), 1);
    Slice configs = burrow__tls_parse_ech_config_list(a, buf, &err);
    if (BURROW_FAILED(err)) {
        arena_free(&ar);
        testing_t_fatal_v(t, err);
    }
    const TlsECHConfig *config = pick(a, configs);
    if (config != NULL) {
        Any v = BURROW_ANY(&burrow__tls_ech_config_type, (void *)(uintptr_t)config);
        Str got = fmt_sprintf_v(a, "%v", v);
        testing_t_errorf_v(t, "got %s, want nil", got);
        arena_free(&ar);
        testing_t_fail_now(t);
    }
    arena_free(&ar);
}

enum { MAX_NAME_LENGTH = 64 };

/* The inner hello TestECHPadding encodes for server_name. */
static Slice encode_inner(Alloc *a, Str server_name, Error *err) {
    static const uint16_t cipher_suites[1] = {TLS_TLS_AES_128_GCM_SHA256};
    static const Byte compression_methods[1] = {0};
    static const uint16_t supported_versions[1] = {TLS_VERSION_TLS13};
    TlsClientHelloMsg inner = burrow__tls_client_hello_msg_zero();
    inner.vers = TLS_VERSION_TLS13;
    inner.random = slice_make(a, TYPE_BYTE, 32, 32);
    if (inner.random.p == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return slice_nil(TYPE_BYTE);
    }
    inner.server_name = server_name;
    inner.cipher_suites =
        slice_from((uint16_t *)(uintptr_t)cipher_suites, 1, 1, TYPE_UINT16);
    inner.compression_methods =
        slice_from((Byte *)(uintptr_t)compression_methods, 1, 1, TYPE_BYTE);
    inner.supported_versions =
        slice_from((uint16_t *)(uintptr_t)supported_versions, 1, 1, TYPE_UINT16);
    return burrow__tls_encode_inner_client_hello(&inner, a, MAX_NAME_LENGTH, err);
}

/* strings.Repeat("a", n) + ".test" */
static Str a_dot_test(Alloc *a, Int n) {
    Str s = strings_repeat(a, BURROW_S("a"), n);
    return fmt_sprintf_v(a, "%s.test", s);
}

typedef struct ECHPaddingCase {
    const char *name;
    Int repeat; /* -1 for no SNI, 0 for "a.test" */
} ECHPaddingCase;

static void ech_padding(void *env, TestingT *t) {
    const ECHPaddingCase *tc = env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str server_name = tc->repeat < 0    ? BURROW_S("")
                      : tc->repeat == 0 ? BURROW_S("a.test")
                                        : a_dot_test(a, tc->repeat);
    Error err = BURROW_NO_ERROR;
    Slice encoded = encode_inner(a, server_name, &err);
    if (BURROW_FAILED(err)) {
        arena_free(&ar);
        testing_t_fatal_v(t, err);
    }
    if (encoded.len % 32 != 0)
        testing_t_errorf_v(t, "got %d, want multiple of 32", encoded.len);
    arena_free(&ar);
}

static void ech_set_size_reduction(void *env, TestingT *t) {
    (void)env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    /* Not in Go: the set of sizes is a list of the distinct ones. */
    Int sizes[MAX_NAME_LENGTH];
    Int nsizes = 0;
    for (Int sni_len = 1; sni_len <= MAX_NAME_LENGTH; sni_len++) {
        Error err = BURROW_NO_ERROR;
        Slice encoded = encode_inner(a, a_dot_test(a, sni_len), &err);
        if (BURROW_FAILED(err)) {
            arena_free(&ar);
            testing_t_fatal_v(t, err);
        }
        bool seen = false;
        for (Int i = 0; i < nsizes; i++)
            seen = seen || sizes[i] == encoded.len;
        if (!seen)
            sizes[nsizes++] = encoded.len;
    }
    if (nsizes > 4)
        testing_t_errorf_v(t,
                           "got %d distinct encoded sizes for SNI lengths 1..%d, want "
                           "<= 4",
                           nsizes, (Int)MAX_NAME_LENGTH);
    arena_free(&ar);
}

static void TestECHPadding(TestingT *t) {
    static const ECHPaddingCase cases[] = {
        {"Short", 0},
        {"Medium", 30},
        {"MaxLength", MAX_NAME_LENGTH},
        {"NoSNI", -1},
    };
    for (size_t i = 0; i < NELEM(cases); i++)
        testing_t_run(
            t, str_from_cstr(cases[i].name),
            BURROW_FN(TestingTFunc, ech_padding, (void *)(uintptr_t)&cases[i]));

    testing_t_run(t, BURROW_S("SetSizeReduction"),
                  BURROW_FN(TestingTFunc, ech_set_size_reduction, NULL));
}

#define TESTS(X)                                                                       \
    X(TestDecodeECHConfigLists)                                                        \
    X(TestSkipBadConfigs)                                                              \
    X(TestPickECHConfigWithInvalidAEADID)                                              \
    X(TestECHPadding)

TESTING_MAIN(TESTS)
