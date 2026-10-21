/* crypto/tls: the cipher suites, what they are made of, and the order they
 * are preferred in.
 *
 * Copyright 2010 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/crypto/tls.h"

#include "burrow/core.h"
#include "burrow/crypto.h"
#include "burrow/crypto/aes.h"
#include "burrow/crypto/cipher.h"
#include "burrow/crypto/des.h"
#include "burrow/crypto/hmac.h"
#include "burrow/crypto/rc4.h"
#include "burrow/crypto/sha256.h"
#include "burrow/error.h"
#include "burrow/hash.h"
#include "burrow/mem.h"
#include "burrow/mem/heap.h"
#include "burrow/pal.h"
#include "burrow/panic.h"
#include "burrow/runtime.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include "aes_internal.h"
#include "chacha20poly1305.h"
#include "crypto_internal.h"
#include "internal.h"
#include "sha1_internal.h"
#include "tls_internal.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* ------------------------------------------------------------ the public ones */

static const uint16_t tls_supported_up_to_tls12[] = {
    TLS_VERSION_TLS10,
    TLS_VERSION_TLS11,
    TLS_VERSION_TLS12,
};
static const uint16_t tls_supported_only_tls12[] = {TLS_VERSION_TLS12};
static const uint16_t tls_supported_only_tls13[] = {TLS_VERSION_TLS13};

/* Which of the three lists above a suite has, so the table can be static. */
enum { TLS_UP_TO_12, TLS_ONLY_12, TLS_ONLY_13 };

typedef struct TlsPublicSuite {
    const char *name;
    int versions;
    uint16_t id;
    bool insecure;
} TlsPublicSuite;

#define TLS_PUBLIC(id, versions, insecure) {#id, versions, TLS_##id, insecure}

static const TlsPublicSuite tls_public_suites[] = {
    TLS_PUBLIC(TLS_AES_128_GCM_SHA256, TLS_ONLY_13, false),
    TLS_PUBLIC(TLS_AES_256_GCM_SHA384, TLS_ONLY_13, false),
    TLS_PUBLIC(TLS_CHACHA20_POLY1305_SHA256, TLS_ONLY_13, false),

    TLS_PUBLIC(TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA, TLS_UP_TO_12, false),
    TLS_PUBLIC(TLS_ECDHE_ECDSA_WITH_AES_256_CBC_SHA, TLS_UP_TO_12, false),
    TLS_PUBLIC(TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA, TLS_UP_TO_12, false),
    TLS_PUBLIC(TLS_ECDHE_RSA_WITH_AES_256_CBC_SHA, TLS_UP_TO_12, false),
    TLS_PUBLIC(TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256, TLS_ONLY_12, false),
    TLS_PUBLIC(TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384, TLS_ONLY_12, false),
    TLS_PUBLIC(TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256, TLS_ONLY_12, false),
    TLS_PUBLIC(TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384, TLS_ONLY_12, false),
    TLS_PUBLIC(TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256, TLS_ONLY_12, false),
    TLS_PUBLIC(TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256, TLS_ONLY_12, false),
};

static const TlsPublicSuite tls_public_insecure_suites[] = {
    TLS_PUBLIC(TLS_RSA_WITH_RC4_128_SHA, TLS_UP_TO_12, true),
    TLS_PUBLIC(TLS_RSA_WITH_3DES_EDE_CBC_SHA, TLS_UP_TO_12, true),
    TLS_PUBLIC(TLS_RSA_WITH_AES_128_CBC_SHA, TLS_UP_TO_12, true),
    TLS_PUBLIC(TLS_RSA_WITH_AES_256_CBC_SHA, TLS_UP_TO_12, true),
    TLS_PUBLIC(TLS_RSA_WITH_AES_128_CBC_SHA256, TLS_ONLY_12, true),
    TLS_PUBLIC(TLS_RSA_WITH_AES_128_GCM_SHA256, TLS_ONLY_12, true),
    TLS_PUBLIC(TLS_RSA_WITH_AES_256_GCM_SHA384, TLS_ONLY_12, true),
    TLS_PUBLIC(TLS_ECDHE_ECDSA_WITH_RC4_128_SHA, TLS_UP_TO_12, true),
    TLS_PUBLIC(TLS_ECDHE_RSA_WITH_RC4_128_SHA, TLS_UP_TO_12, true),
    TLS_PUBLIC(TLS_ECDHE_RSA_WITH_3DES_EDE_CBC_SHA, TLS_UP_TO_12, true),
    TLS_PUBLIC(TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA256, TLS_ONLY_12, true),
    TLS_PUBLIC(TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA256, TLS_ONLY_12, true),
};

#undef TLS_PUBLIC

#define TLS_NPUBLIC ((Int)(sizeof tls_public_suites / sizeof tls_public_suites[0]))
#define TLS_NPUBLIC_INSECURE                                                           \
    ((Int)(sizeof tls_public_insecure_suites / sizeof tls_public_insecure_suites[0]))

/* A fresh copy of one of the version lists, nil when a is out of memory. */
static Slice tls_versions_slice(Alloc *a, int versions) {
    const uint16_t *v = tls_supported_only_tls13;
    Int n = 1;
    if (versions == TLS_UP_TO_12) {
        v = tls_supported_up_to_tls12;
        n = 3;
    } else if (versions == TLS_ONLY_12) {
        v = tls_supported_only_tls12;
    }
    Slice s = slice_make(a, TYPE_UINT16, n, n);
    if (s.p != NULL)
        memcpy(s.p, v, (size_t)n * sizeof v[0]);
    return s;
}

/* A fresh Slice of pointers to fresh CipherSuites, as Go makes on every call. */
static Slice tls_public_list(Alloc *a, const TlsPublicSuite *list, Int n) {
    TlsCipherSuite *cs = BURROW_NEW_N(a, TlsCipherSuite, (size_t)n);
    if (cs == NULL)
        return slice_nil(TYPE_TLS_CIPHER_SUITE_PTR);
    Slice s = slice_make(a, TYPE_TLS_CIPHER_SUITE_PTR, n, n);
    if (s.p == NULL)
        return s;
    TlsCipherSuitePtr *p = (TlsCipherSuitePtr *)s.p;
    for (Int i = 0; i < n; i++) {
        cs[i].id = list[i].id;
        cs[i].name = str_from_cstr(list[i].name);
        cs[i].supported_versions = tls_versions_slice(a, list[i].versions);
        if (cs[i].supported_versions.p == NULL)
            return slice_nil(TYPE_TLS_CIPHER_SUITE_PTR);
        cs[i].insecure = list[i].insecure;
        p[i] = &cs[i];
    }
    return s;
}

Slice tls_cipher_suites(Alloc *a) {
    return tls_public_list(a, tls_public_suites, TLS_NPUBLIC);
}

Slice tls_insecure_cipher_suites(Alloc *a) {
    return tls_public_list(a, tls_public_insecure_suites, TLS_NPUBLIC_INSECURE);
}

Str tls_cipher_suite_name(Alloc *a, uint16_t id) {
    for (Int i = 0; i < TLS_NPUBLIC; i++) {
        if (tls_public_suites[i].id == id)
            return str_from_cstr(tls_public_suites[i].name);
    }
    for (Int i = 0; i < TLS_NPUBLIC_INSECURE; i++) {
        if (tls_public_insecure_suites[i].id == id)
            return str_from_cstr(tls_public_insecure_suites[i].name);
    }
    return burrow__tls_hex4_string(a, id);
}

/* ----------------------------------------------------------------- ciphers */

bool burrow__tls_cipher_is_nil(TlsCipher c) {
    return c.stream.vt == NULL && c.mode.vt == NULL && c.aead.vt == NULL;
}

/* Go panics with the error from a constructor it does not expect to fail. An
 * out of memory is handed back as a nil cipher instead, and anything else
 * panics here, kept on the heap since the panic leaves this frame. */
static void tls_check_error(Error err) {
    if (!BURROW_FAILED(err) || errors_is(err, burrow_err_out_of_memory))
        return;
    Error *box = BURROW_NEW(heap_allocator(), Error);
    if (box == NULL)
        panic_str(error_text(err));
    *box = error_retain(heap_allocator(), err);
    panic((Any){TYPE_ERROR, box});
}

static TlsCipher tls_cipher_rc4(Alloc *a, Slice key, Slice iv, bool is_read) {
    (void)iv;
    (void)is_read;
    TlsCipher c = {0};
    Error err = BURROW_NO_ERROR;
    Rc4Cipher *rc = rc4_new_cipher(a, key, &err);
    if (rc != NULL)
        c.stream = rc4_cipher_as_cipher_stream(rc);
    return c;
}

static TlsCipher tls_cbc_cipher(Alloc *a, CipherBlock block, Error err, Slice iv,
                                bool is_read) {
    TlsCipher c = {0};
    tls_check_error(err);
    if (block.vt == NULL)
        return c;
    if (is_read)
        c.mode = cipher_new_cbc_decrypter(a, block, iv);
    else
        c.mode = cipher_new_cbc_encrypter(a, block, iv);
    return c;
}

static TlsCipher tls_cipher_3des(Alloc *a, Slice key, Slice iv, bool is_read) {
    Error err = BURROW_NO_ERROR;
    CipherBlock block = des_new_triple_des_cipher(a, key, &err);
    return tls_cbc_cipher(a, block, err, iv, is_read);
}

static TlsCipher tls_cipher_aes(Alloc *a, Slice key, Slice iv, bool is_read) {
    Error err = BURROW_NO_ERROR;
    CipherBlock block = aes_new_cipher(a, key, &err);
    return tls_cbc_cipher(a, block, err, iv, is_read);
}

static Hash tls_mac_sha1(Alloc *a, Slice key) {
    return hmac_new(a, burrow__sha1_new_constant_time, key);
}

static Hash tls_mac_sha256(Alloc *a, Slice key) {
    return hmac_new(a, sha256_new, key);
}

/* prefixNonceAEAD and xorNonceAEAD, which are the same shape. */
typedef struct TlsNonceAEAD {
    Byte nonce[TLS_AEAD_NONCE_LENGTH];
    CipherAEAD aead;
} TlsNonceAEAD;

static Int tls_prefix_nonce_size(void *self) {
    (void)self;
    return TLS_AEAD_NONCE_LENGTH - TLS_NONCE_PREFIX_LENGTH;
}

static Int tls_nonce_aead_overhead(void *self) {
    return cipher_aead_overhead(((TlsNonceAEAD *)self)->aead);
}

static Slice tls_nonce_slice(TlsNonceAEAD *f) {
    return slice_from(f->nonce, TLS_AEAD_NONCE_LENGTH, TLS_AEAD_NONCE_LENGTH,
                      TYPE_BYTE);
}

/* copy(f.nonce[4:], nonce) */
static void tls_prefix_nonce_copy(TlsNonceAEAD *f, Slice nonce) {
    Int n = nonce.len < 8 ? nonce.len : 8;
    if (n > 0)
        memcpy(f->nonce + 4, nonce.p, (size_t)n);
}

static Slice tls_prefix_nonce_seal(void *self, Alloc *a, Slice dst, Slice nonce,
                                   Slice plaintext, Slice additional_data) {
    TlsNonceAEAD *f = (TlsNonceAEAD *)self;
    tls_prefix_nonce_copy(f, nonce);
    return cipher_aead_seal(f->aead, a, dst, tls_nonce_slice(f), plaintext,
                            additional_data);
}

static Slice tls_prefix_nonce_open(void *self, Alloc *a, Slice dst, Slice nonce,
                                   Slice ciphertext, Slice additional_data,
                                   Error *err) {
    TlsNonceAEAD *f = (TlsNonceAEAD *)self;
    tls_prefix_nonce_copy(f, nonce);
    return cipher_aead_open(f->aead, a, dst, tls_nonce_slice(f), ciphertext,
                            additional_data, err);
}

static const CipherAEADVT tls_prefix_nonce_vt = {
    NULL,
    tls_prefix_nonce_size,
    tls_nonce_aead_overhead,
    tls_prefix_nonce_seal,
    tls_prefix_nonce_open,
};

static Int tls_xor_nonce_size(void *self) {
    (void)self;
    return 8; /* 64-bit sequence number */
}

/* for i, b := range nonce { f.nonceMask[4+i] ^= b } */
static void tls_xor_nonce_mask(TlsNonceAEAD *f, Slice nonce) {
    if (nonce.len > 8)
        runtime_index_out_of_range(TLS_AEAD_NONCE_LENGTH, TLS_AEAD_NONCE_LENGTH);
    const Byte *b = (const Byte *)nonce.p;
    for (Int i = 0; i < nonce.len; i++)
        f->nonce[4 + i] ^= b[i];
}

static Slice tls_xor_nonce_seal(void *self, Alloc *a, Slice dst, Slice nonce,
                                Slice plaintext, Slice additional_data) {
    TlsNonceAEAD *f = (TlsNonceAEAD *)self;
    tls_xor_nonce_mask(f, nonce);
    Slice result = cipher_aead_seal(f->aead, a, dst, tls_nonce_slice(f), plaintext,
                                    additional_data);
    tls_xor_nonce_mask(f, nonce);
    return result;
}

static Slice tls_xor_nonce_open(void *self, Alloc *a, Slice dst, Slice nonce,
                                Slice ciphertext, Slice additional_data, Error *err) {
    TlsNonceAEAD *f = (TlsNonceAEAD *)self;
    tls_xor_nonce_mask(f, nonce);
    Slice result = cipher_aead_open(f->aead, a, dst, tls_nonce_slice(f), ciphertext,
                                    additional_data, err);
    tls_xor_nonce_mask(f, nonce);
    return result;
}

static const CipherAEADVT tls_xor_nonce_vt = {
    NULL,
    tls_xor_nonce_size,
    tls_nonce_aead_overhead,
    tls_xor_nonce_seal,
    tls_xor_nonce_open,
};

/* Wraps aead, which may be nil from an out of memory, with the nonce. */
static TlsCipher tls_wrap_aead(Alloc *a, CipherAEAD aead, const CipherAEADVT *vt,
                               Slice nonce) {
    TlsCipher c = {0};
    if (aead.vt == NULL)
        return c;
    TlsNonceAEAD *f = BURROW_NEW(a, TlsNonceAEAD);
    if (f == NULL)
        return c;
    f->aead = aead;
    memcpy(f->nonce, nonce.p, (size_t)nonce.len);
    c.aead = (CipherAEAD){vt, f};
    c.explicit_nonce_len = vt == &tls_prefix_nonce_vt ? tls_prefix_nonce_size(f) : 0;
    return c;
}

/* aes.NewCipher and then GCM over it, from gcm.NewGCMForTLS12 or
 * NewGCMForTLS13. */
static CipherAEAD tls_new_aes_gcm(Alloc *a, Slice key,
                                  CipherAEAD (*new_gcm)(Alloc *, CipherBlock,
                                                        Error *)) {
    Error err = BURROW_NO_ERROR;
    CipherBlock block = aes_new_cipher(a, key, &err);
    tls_check_error(err);
    if (block.vt == NULL)
        return (CipherAEAD){NULL, NULL};
    CipherAEAD aead = new_gcm(a, block, &err);
    tls_check_error(err);
    return aead;
}

static TlsCipher tls_aead_aes_gcm(Alloc *a, Slice key, Slice nonce_prefix) {
    if (nonce_prefix.len != TLS_NONCE_PREFIX_LENGTH)
        panic_str(BURROW_S("tls: internal error: wrong nonce length"));
    return tls_wrap_aead(a, tls_new_aes_gcm(a, key, burrow__gcm_new_for_tls12),
                         &tls_prefix_nonce_vt, nonce_prefix);
}

static TlsCipher tls_aead_aes_gcm_tls13(Alloc *a, Slice key, Slice nonce_mask) {
    if (nonce_mask.len != TLS_AEAD_NONCE_LENGTH)
        panic_str(BURROW_S("tls: internal error: wrong nonce length"));
    return tls_wrap_aead(a, tls_new_aes_gcm(a, key, burrow__gcm_new_for_tls13),
                         &tls_xor_nonce_vt, nonce_mask);
}

static TlsCipher tls_aead_chacha20_poly1305(Alloc *a, Slice key, Slice nonce_mask) {
    if (nonce_mask.len != TLS_AEAD_NONCE_LENGTH)
        panic_str(BURROW_S("tls: internal error: wrong nonce length"));
    Error err = BURROW_NO_ERROR;
    CipherAEAD aead = chacha20poly1305_new(a, key, &err);
    tls_check_error(err);
    return tls_wrap_aead(a, aead, &tls_xor_nonce_vt, nonce_mask);
}

Slice burrow__tls10_mac(Alloc *a, Hash h, Slice out, Slice seq, Slice header,
                        Slice data, Slice extra) {
    Error err = BURROW_NO_ERROR;
    hash_reset(h);
    hash_write(h, seq, &err);
    hash_write(h, header, &err);
    hash_write(h, data, &err);
    Slice res = hash_sum(a, h, out);
    if (!slice_is_nil(extra))
        hash_write(h, extra, &err);
    return res;
}

/* ------------------------------------------------------------------ tables */

const TlsSuite burrow__tls_cipher_suites[] = {
    {TLS_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256, 32, 0, 12, TLS_KA_ECDHE_RSA,
     TLS_SUITE_ECDHE | TLS_SUITE_TLS12, NULL, NULL, tls_aead_chacha20_poly1305},
    {TLS_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256, 32, 0, 12, TLS_KA_ECDHE_ECDSA,
     TLS_SUITE_ECDHE | TLS_SUITE_EC_SIGN | TLS_SUITE_TLS12, NULL, NULL,
     tls_aead_chacha20_poly1305},
    {TLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256, 16, 0, 4, TLS_KA_ECDHE_RSA,
     TLS_SUITE_ECDHE | TLS_SUITE_TLS12, NULL, NULL, tls_aead_aes_gcm},
    {TLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256, 16, 0, 4, TLS_KA_ECDHE_ECDSA,
     TLS_SUITE_ECDHE | TLS_SUITE_EC_SIGN | TLS_SUITE_TLS12, NULL, NULL,
     tls_aead_aes_gcm},
    {TLS_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384, 32, 0, 4, TLS_KA_ECDHE_RSA,
     TLS_SUITE_ECDHE | TLS_SUITE_TLS12 | TLS_SUITE_SHA384, NULL, NULL,
     tls_aead_aes_gcm},
    {TLS_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384, 32, 0, 4, TLS_KA_ECDHE_ECDSA,
     TLS_SUITE_ECDHE | TLS_SUITE_EC_SIGN | TLS_SUITE_TLS12 | TLS_SUITE_SHA384, NULL,
     NULL, tls_aead_aes_gcm},
    {TLS_TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA256, 16, 32, 16, TLS_KA_ECDHE_RSA,
     TLS_SUITE_ECDHE | TLS_SUITE_TLS12, tls_cipher_aes, tls_mac_sha256, NULL},
    {TLS_TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA, 16, 20, 16, TLS_KA_ECDHE_RSA,
     TLS_SUITE_ECDHE, tls_cipher_aes, tls_mac_sha1, NULL},
    {TLS_TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA256, 16, 32, 16, TLS_KA_ECDHE_ECDSA,
     TLS_SUITE_ECDHE | TLS_SUITE_EC_SIGN | TLS_SUITE_TLS12, tls_cipher_aes,
     tls_mac_sha256, NULL},
    {TLS_TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA, 16, 20, 16, TLS_KA_ECDHE_ECDSA,
     TLS_SUITE_ECDHE | TLS_SUITE_EC_SIGN, tls_cipher_aes, tls_mac_sha1, NULL},
    {TLS_TLS_ECDHE_RSA_WITH_AES_256_CBC_SHA, 32, 20, 16, TLS_KA_ECDHE_RSA,
     TLS_SUITE_ECDHE, tls_cipher_aes, tls_mac_sha1, NULL},
    {TLS_TLS_ECDHE_ECDSA_WITH_AES_256_CBC_SHA, 32, 20, 16, TLS_KA_ECDHE_ECDSA,
     TLS_SUITE_ECDHE | TLS_SUITE_EC_SIGN, tls_cipher_aes, tls_mac_sha1, NULL},
    {TLS_TLS_RSA_WITH_AES_128_GCM_SHA256, 16, 0, 4, TLS_KA_RSA, TLS_SUITE_TLS12, NULL,
     NULL, tls_aead_aes_gcm},
    {TLS_TLS_RSA_WITH_AES_256_GCM_SHA384, 32, 0, 4, TLS_KA_RSA,
     TLS_SUITE_TLS12 | TLS_SUITE_SHA384, NULL, NULL, tls_aead_aes_gcm},
    {TLS_TLS_RSA_WITH_AES_128_CBC_SHA256, 16, 32, 16, TLS_KA_RSA, TLS_SUITE_TLS12,
     tls_cipher_aes, tls_mac_sha256, NULL},
    {TLS_TLS_RSA_WITH_AES_128_CBC_SHA, 16, 20, 16, TLS_KA_RSA, 0, tls_cipher_aes,
     tls_mac_sha1, NULL},
    {TLS_TLS_RSA_WITH_AES_256_CBC_SHA, 32, 20, 16, TLS_KA_RSA, 0, tls_cipher_aes,
     tls_mac_sha1, NULL},
    {TLS_TLS_ECDHE_RSA_WITH_3DES_EDE_CBC_SHA, 24, 20, 8, TLS_KA_ECDHE_RSA,
     TLS_SUITE_ECDHE, tls_cipher_3des, tls_mac_sha1, NULL},
    {TLS_TLS_RSA_WITH_3DES_EDE_CBC_SHA, 24, 20, 8, TLS_KA_RSA, 0, tls_cipher_3des,
     tls_mac_sha1, NULL},
    {TLS_TLS_RSA_WITH_RC4_128_SHA, 16, 20, 0, TLS_KA_RSA, 0, tls_cipher_rc4,
     tls_mac_sha1, NULL},
    {TLS_TLS_ECDHE_RSA_WITH_RC4_128_SHA, 16, 20, 0, TLS_KA_ECDHE_RSA, TLS_SUITE_ECDHE,
     tls_cipher_rc4, tls_mac_sha1, NULL},
    {TLS_TLS_ECDHE_ECDSA_WITH_RC4_128_SHA, 16, 20, 0, TLS_KA_ECDHE_ECDSA,
     TLS_SUITE_ECDHE | TLS_SUITE_EC_SIGN, tls_cipher_rc4, tls_mac_sha1, NULL},
};

const Int burrow__tls_ncipher_suites =
    (Int)(sizeof burrow__tls_cipher_suites / sizeof burrow__tls_cipher_suites[0]);

const TlsSuiteTLS13 burrow__tls_cipher_suites_tls13[] = {
    {TLS_TLS_AES_128_GCM_SHA256, 16, tls_aead_aes_gcm_tls13, CRYPTO_SHA256},
    {TLS_TLS_CHACHA20_POLY1305_SHA256, 32, tls_aead_chacha20_poly1305, CRYPTO_SHA256},
    {TLS_TLS_AES_256_GCM_SHA384, 32, tls_aead_aes_gcm_tls13, CRYPTO_SHA384},
};

const Int burrow__tls_ncipher_suites_tls13 =
    (Int)(sizeof burrow__tls_cipher_suites_tls13 /
          sizeof burrow__tls_cipher_suites_tls13[0]);

/* cipherSuitesPreferenceOrder is the order in which we'll select (on the
 * server) or advertise (on the client) TLS 1.0–1.2 cipher suites.
 *
 * Cipher suites are filtered but not reordered based on the application and
 * peer's preferences, meaning we'll never select a suite lower in this list if
 * any higher one is available. This makes it more defensible to keep weaker
 * cipher suites enabled, especially on the server side where we get the last
 * word, since there are no known downgrade attacks on cipher suites selection.
 *
 * The list is sorted by applying the following priority rules, stopping at the
 * first (most important) applicable one:
 *
 *   - Anything else comes before RC4
 *
 *     RC4 has practically exploitable biases. See https://www.rc4nomore.com.
 *
 *   - Anything else comes before CBC_SHA256
 *
 *     SHA-256 variants of the CBC ciphersuites don't implement any Lucky13
 *     countermeasures. See https://www.isg.rhul.ac.uk/tls/Lucky13.html and
 *     https://www.imperialviolet.org/2013/02/04/luckythirteen.html.
 *
 *   - Anything else comes before 3DES
 *
 *     3DES has 64-bit blocks, which makes it fundamentally susceptible to
 *     birthday attacks. See https://sweet32.info.
 *
 *   - ECDHE comes before anything else
 *
 *     Once we got the broken stuff out of the way, the most important
 *     property a cipher suite can have is forward secrecy. We don't
 *     implement FFDHE, so that means ECDHE.
 *
 *   - AEADs come before CBC ciphers
 *
 *     Even with Lucky13 countermeasures, MAC-then-Encrypt CBC cipher suites
 *     are fundamentally fragile, and suffered from an endless sequence of
 *     padding oracle attacks. See https://eprint.iacr.org/2015/1129,
 *     https://www.imperialviolet.org/2014/12/08/poodleagain.html, and
 *     https://blog.cloudflare.com/yet-another-padding-oracle-in-openssl-cbc-ciphersuites/.
 *
 *   - AES comes before ChaCha20
 *
 *     When AES hardware is available, AES-128-GCM and AES-256-GCM are faster
 *     than ChaCha20Poly1305.
 *
 *     When AES hardware is not available, AES-128-GCM is one or more of: much
 *     slower, way more complex, and less safe (because not constant time)
 *     than ChaCha20Poly1305.
 *
 *     We use this list if we think both peers have AES hardware, and
 *     cipherSuitesPreferenceOrderNoAES otherwise.
 *
 *   - AES-128 comes before AES-256
 *
 *     The only potential advantages of AES-256 are better multi-target
 *     margins, and hypothetical post-quantum properties. Neither apply to
 *     TLS, and AES-256 is slower due to its four extra rounds (which don't
 *     contribute to the advantages above).
 *
 *   - ECDSA comes before RSA
 *
 *     The relative order of ECDSA and RSA cipher suites doesn't matter,
 *     as they depend on the certificate. Pick one to get a stable order. */
const uint16_t burrow__tls_cipher_suites_preference_order[] = {
    /* AEADs w/ ECDHE */
    TLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
    TLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
    TLS_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
    TLS_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
    TLS_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256,
    TLS_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,

    /* CBC w/ ECDHE */
    TLS_TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA,
    TLS_TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA,
    TLS_TLS_ECDHE_ECDSA_WITH_AES_256_CBC_SHA,
    TLS_TLS_ECDHE_RSA_WITH_AES_256_CBC_SHA,

    /* AEADs w/o ECDHE */
    TLS_TLS_RSA_WITH_AES_128_GCM_SHA256,
    TLS_TLS_RSA_WITH_AES_256_GCM_SHA384,

    /* CBC w/o ECDHE */
    TLS_TLS_RSA_WITH_AES_128_CBC_SHA,
    TLS_TLS_RSA_WITH_AES_256_CBC_SHA,

    /* 3DES */
    TLS_TLS_ECDHE_RSA_WITH_3DES_EDE_CBC_SHA,
    TLS_TLS_RSA_WITH_3DES_EDE_CBC_SHA,

    /* CBC_SHA256 */
    TLS_TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA256,
    TLS_TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA256,
    TLS_TLS_RSA_WITH_AES_128_CBC_SHA256,

    /* RC4 */
    TLS_TLS_ECDHE_ECDSA_WITH_RC4_128_SHA,
    TLS_TLS_ECDHE_RSA_WITH_RC4_128_SHA,
    TLS_TLS_RSA_WITH_RC4_128_SHA,
};

const uint16_t burrow__tls_cipher_suites_preference_order_no_aes[] = {
    /* ChaCha20Poly1305 */
    TLS_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256,
    TLS_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,

    /* AES-GCM w/ ECDHE */
    TLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
    TLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
    TLS_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
    TLS_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,

    /* The rest of cipherSuitesPreferenceOrder. */
    TLS_TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA,
    TLS_TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA,
    TLS_TLS_ECDHE_ECDSA_WITH_AES_256_CBC_SHA,
    TLS_TLS_ECDHE_RSA_WITH_AES_256_CBC_SHA,
    TLS_TLS_RSA_WITH_AES_128_GCM_SHA256,
    TLS_TLS_RSA_WITH_AES_256_GCM_SHA384,
    TLS_TLS_RSA_WITH_AES_128_CBC_SHA,
    TLS_TLS_RSA_WITH_AES_256_CBC_SHA,
    TLS_TLS_ECDHE_RSA_WITH_3DES_EDE_CBC_SHA,
    TLS_TLS_RSA_WITH_3DES_EDE_CBC_SHA,
    TLS_TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA256,
    TLS_TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA256,
    TLS_TLS_RSA_WITH_AES_128_CBC_SHA256,
    TLS_TLS_ECDHE_ECDSA_WITH_RC4_128_SHA,
    TLS_TLS_ECDHE_RSA_WITH_RC4_128_SHA,
    TLS_TLS_RSA_WITH_RC4_128_SHA,
};

const Int burrow__tls_npreference_order =
    (Int)(sizeof burrow__tls_cipher_suites_preference_order /
          sizeof burrow__tls_cipher_suites_preference_order[0]);
const Int burrow__tls_npreference_order_no_aes =
    (Int)(sizeof burrow__tls_cipher_suites_preference_order_no_aes /
          sizeof burrow__tls_cipher_suites_preference_order_no_aes[0]);

const uint16_t burrow__tls_default_cipher_suites_tls13[3] = {
    TLS_TLS_AES_128_GCM_SHA256,
    TLS_TLS_AES_256_GCM_SHA384,
    TLS_TLS_CHACHA20_POLY1305_SHA256,
};

const uint16_t burrow__tls_default_cipher_suites_tls13_no_aes[3] = {
    TLS_TLS_CHACHA20_POLY1305_SHA256,
    TLS_TLS_AES_128_GCM_SHA256,
    TLS_TLS_AES_256_GCM_SHA384,
};

bool burrow__tls_disabled_cipher_suite(uint16_t id) {
    switch (id) {
    /* CBC_SHA256 */
    case TLS_TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA256:
    case TLS_TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA256:
    case TLS_TLS_RSA_WITH_AES_128_CBC_SHA256:
    /* RC4 */
    case TLS_TLS_ECDHE_ECDSA_WITH_RC4_128_SHA:
    case TLS_TLS_ECDHE_RSA_WITH_RC4_128_SHA:
    case TLS_TLS_RSA_WITH_RC4_128_SHA:
    /* RSA key exchange */
    case TLS_TLS_RSA_WITH_3DES_EDE_CBC_SHA:
    case TLS_TLS_RSA_WITH_AES_128_CBC_SHA:
    case TLS_TLS_RSA_WITH_AES_256_CBC_SHA:
    case TLS_TLS_RSA_WITH_AES_128_GCM_SHA256:
    case TLS_TLS_RSA_WITH_AES_256_GCM_SHA384:
    /* 3DES */
    case TLS_TLS_ECDHE_RSA_WITH_3DES_EDE_CBC_SHA:
        return true;
    default:
        return false;
    }
}

/* Go's test is for the assembly it has. burrow has hardware AES-GCM for x86
 * and arm64 only, so ppc64 and s390x are false here where Go has them true. */
int burrow__tls_aes_gcm_hardware_override = -1;

bool burrow__tls_has_aes_gcm_hardware_support(void) {
    if (burrow__tls_aes_gcm_hardware_override >= 0)
        return burrow__tls_aes_gcm_hardware_override != 0;
#if defined(CRYPTO_X86_AES)
    uint32_t need =
        PAL_CPU_X86_AES | PAL_CPU_X86_PCLMULQDQ | PAL_CPU_X86_SSE41 | PAL_CPU_X86_SSSE3;
    return (pal_cpu_features() & need) == need;
#elif defined(CRYPTO_ARM64_AES)
    uint32_t need = PAL_CPU_ARM64_AES | PAL_CPU_ARM64_PMULL;
    return (pal_cpu_features() & need) == need;
#else
    return false;
#endif
}

/* aesgcmCiphers represents the cipher suites that use AES-GCM. */
static bool tls_aes_gcm_cipher(uint16_t id) {
    switch (id) {
    /* TLS 1.2 */
    case TLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256:
    case TLS_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384:
    case TLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256:
    case TLS_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384:
    /* TLS 1.3 */
    case TLS_TLS_AES_128_GCM_SHA256:
    case TLS_TLS_AES_256_GCM_SHA384:
        return true;
    default:
        return false;
    }
}

bool burrow__tls_is_aes_gcm_preferred(const uint16_t *ciphers, Int n) {
    if (!burrow__tls_has_aes_gcm_hardware_support())
        return false;
    for (Int i = 0; i < n; i++) {
        if (burrow__tls_cipher_suite_by_id(ciphers[i]) != NULL)
            return tls_aes_gcm_cipher(ciphers[i]);
        if (burrow__tls_cipher_suite_tls13_by_id(ciphers[i]) != NULL)
            return tls_aes_gcm_cipher(ciphers[i]);
    }
    return false;
}

Slice burrow__tls_supported_cipher_suites(Alloc *a, bool aes_gcm_preferred) {
    const uint16_t *order = aes_gcm_preferred
                                ? burrow__tls_cipher_suites_preference_order
                                : burrow__tls_cipher_suites_preference_order_no_aes;
    Slice s = slice_make(a, TYPE_UINT16, burrow__tls_npreference_order,
                         burrow__tls_npreference_order);
    if (s.p == NULL)
        return s;
    memcpy(s.p, order, sizeof(uint16_t) * (size_t)burrow__tls_npreference_order);
    return s;
}

Slice burrow__tls_default_cipher_suites(Alloc *a, bool aes_gcm_preferred) {
    Slice s = burrow__tls_supported_cipher_suites(a, aes_gcm_preferred);
    if (s.p == NULL)
        return s;
    uint16_t *p = (uint16_t *)s.p;
    Int n = 0;
    for (Int i = 0; i < s.len; i++) {
        if (!burrow__tls_disabled_cipher_suite(p[i]))
            p[n++] = p[i];
    }
    /* slices.DeleteFunc clears the tail. */
    memset(p + n, 0, sizeof(uint16_t) * (size_t)(s.len - n));
    s.len = n;
    return s;
}

const TlsSuite *burrow__tls_cipher_suite_by_id(uint16_t id) {
    for (Int i = 0; i < burrow__tls_ncipher_suites; i++) {
        if (burrow__tls_cipher_suites[i].id == id)
            return &burrow__tls_cipher_suites[i];
    }
    return NULL;
}

const TlsSuiteTLS13 *burrow__tls_cipher_suite_tls13_by_id(uint16_t id) {
    for (Int i = 0; i < burrow__tls_ncipher_suites_tls13; i++) {
        if (burrow__tls_cipher_suites_tls13[i].id == id)
            return &burrow__tls_cipher_suites_tls13[i];
    }
    return NULL;
}

const TlsSuite *burrow__tls_mutual_cipher_suite(const uint16_t *have, Int n,
                                                uint16_t want) {
    for (Int i = 0; i < n; i++) {
        if (have[i] == want)
            return burrow__tls_cipher_suite_by_id(have[i]);
    }
    return NULL;
}

const TlsSuiteTLS13 *burrow__tls_mutual_cipher_suite_tls13(const uint16_t *have, Int n,
                                                           uint16_t want) {
    for (Int i = 0; i < n; i++) {
        if (have[i] == want)
            return burrow__tls_cipher_suite_tls13_by_id(have[i]);
    }
    return NULL;
}

const TlsSuite *burrow__tls_select_cipher_suite(const uint16_t *ids, Int nids,
                                                const uint16_t *supported_ids,
                                                Int nsupported,
                                                bool (*ok)(const TlsSuite *, void *),
                                                void *env) {
    for (Int i = 0; i < nids; i++) {
        const TlsSuite *candidate = burrow__tls_cipher_suite_by_id(ids[i]);
        if (candidate == NULL || !ok(candidate, env))
            continue;
        for (Int j = 0; j < nsupported; j++) {
            if (ids[i] == supported_ids[j])
                return candidate;
        }
    }
    return NULL;
}

HashNewFunc burrow__tls_suite13_hash_new(const TlsSuiteTLS13 *c) {
    return burrow__crypto_hash_new_func(c->hash);
}
