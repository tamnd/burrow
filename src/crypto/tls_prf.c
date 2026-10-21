/* crypto/tls: the TLS 1.0 to 1.2 pseudo-random functions, the master secret,
 * the connection keys, the Finished hashes and exported keying material.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/crypto/tls.h"

#include "burrow/bytes.h"
#include "burrow/core.h"
#include "burrow/crypto.h"
#include "burrow/crypto/hmac.h"
#include "burrow/crypto/md5.h"
#include "burrow/crypto/sha1.h"
#include "burrow/crypto/sha256.h"
#include "burrow/crypto/sha512.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/hash.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/panic.h"
#include "burrow/slice.h"

#include "crypto_internal.h"
#include "fips140_module.h"
#include "tls_internal.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* The largest sum of any hash here, SHA-384's being the largest in use. */
#define TLS_PRF_MAX_SUM 64

CRYPTO_FIPS_TEXT void burrow__tls_split_pre_master_secret(Slice secret, Slice *s1,
                                                          Slice *s2) {
    *s1 = slice_sub(secret, 0, (secret.len + 1) / 2);
    *s2 = slice_sub(secret, secret.len / 2, secret.len);
}

/* pHash implements the P_hash function, as defined in RFC 4346, Section 5.
 * The HMAC comes from scratch. False when that is out of memory. */
CRYPTO_FIPS_TEXT static bool tls_p_hash(Alloc *scratch, HashNewFunc hash, Byte *result,
                                        Int n, Slice secret, Slice seed) {
    Byte abuf[TLS_PRF_MAX_SUM], bbuf[TLS_PRF_MAX_SUM];
    Error err = BURROW_NO_ERROR;
    Hash h = hmac_new(scratch, hash, secret);
    if (h.vt == NULL)
        return false;
    hash_write(h, seed, &err);
    Slice a = hash_sum(scratch, h, slice_from(abuf, 0, TLS_PRF_MAX_SUM, TYPE_BYTE));
    while (n > 0) {
        hash_reset(h);
        hash_write(h, a, &err);
        hash_write(h, seed, &err);
        Slice b = hash_sum(scratch, h, slice_from(bbuf, 0, TLS_PRF_MAX_SUM, TYPE_BYTE));
        Int k = b.len < n ? b.len : n;
        memcpy(result, b.p, (size_t)k);
        result += k;
        n -= k;

        hash_reset(h);
        hash_write(h, a, &err);
        a = hash_sum(scratch, h, slice_from(abuf, 0, TLS_PRF_MAX_SUM, TYPE_BYTE));
    }
    return true;
}

/* label and then seed, from scratch. */
CRYPTO_FIPS_TEXT static Slice tls_label_and_seed(Alloc *scratch, Str label,
                                                 Slice seed) {
    Slice s =
        slice_make(scratch, TYPE_BYTE, label.len + seed.len, label.len + seed.len);
    if (s.p == NULL)
        return s;
    if (label.len > 0)
        memcpy(s.p, label.p, (size_t)label.len);
    if (seed.len > 0)
        memcpy((Byte *)s.p + label.len, seed.p, (size_t)seed.len);
    return s;
}

CRYPTO_FIPS_TEXT Slice burrow__tls_prf10(Alloc *a, Slice secret, Str label, Slice seed,
                                         Int key_len) {
    Slice result = slice_make(a, TYPE_BYTE, key_len, key_len);
    if (result.p == NULL && key_len > 0)
        return slice_nil(TYPE_BYTE);
    Arena scratch;
    arena_init(&scratch, NULL, 0);
    Alloc *s = arena_allocator(&scratch);
    bool ok = false;
    Slice label_and_seed = tls_label_and_seed(s, label, seed);
    Byte *result2 = (Byte *)mem_alloc(s, (size_t)key_len + 1, 1);
    if ((label_and_seed.p != NULL || label_and_seed.len == 0) && result2 != NULL) {
        Slice s1, s2;
        burrow__tls_split_pre_master_secret(secret, &s1, &s2);
        ok = tls_p_hash(s, md5_new, (Byte *)result.p, key_len, s1, label_and_seed) &&
             tls_p_hash(s, sha1_new, result2, key_len, s2, label_and_seed);
        for (Int i = 0; ok && i < key_len; i++)
            ((Byte *)result.p)[i] ^= result2[i];
    }
    arena_free(&scratch);
    return ok ? result : slice_nil(TYPE_BYTE);
}

/* tls12.PRF: the TLS 1.2 pseudo-random function, as defined in RFC 5246,
 * Section 5 and allowed by SP 800-135, Revision 1, Section 4.2.2. */
CRYPTO_FIPS_TEXT Slice burrow__tls_prf12(Alloc *a, HashNewFunc h, Slice secret,
                                         Str label, Slice seed, Int key_len) {
    Slice result = slice_make(a, TYPE_BYTE, key_len, key_len);
    if (result.p == NULL && key_len > 0)
        return slice_nil(TYPE_BYTE);
    Arena scratch;
    arena_init(&scratch, NULL, 0);
    Alloc *s = arena_allocator(&scratch);
    bool ok = false;
    Slice label_and_seed = tls_label_and_seed(s, label, seed);
    if (label_and_seed.p != NULL || label_and_seed.len == 0)
        ok = tls_p_hash(s, h, (Byte *)result.p, key_len, secret, label_and_seed);
    arena_free(&scratch);
    return ok ? result : slice_nil(TYPE_BYTE);
}

CRYPTO_FIPS_TEXT Slice burrow__tls12_master_secret(Alloc *a, HashNewFunc hash,
                                                   Slice pre_master_secret,
                                                   Slice transcript) {
    /* "The TLS 1.2 KDF is an approved KDF when the following conditions are
     * satisfied: [...] (3) P_HASH uses either SHA-256, SHA-384 or SHA-512." */
    Arena scratch;
    arena_init(&scratch, NULL, 0);
    Hash h = hash(arena_allocator(&scratch));
    if (h.vt == NULL) {
        arena_free(&scratch);
        return slice_nil(TYPE_BYTE);
    }
    /* Not in Go: the 46 is Go's, so SHA-384, whose size is 48, is recorded as
     * not approved here, as it is in Go. */
    if (burrow__sha256_is_digest(h)) {
        if (hash_size(h) != 32)
            burrow__fips140_record_non_approved();
    } else if (burrow__sha512_is_digest(h)) {
        if (hash_size(h) != 46 && hash_size(h) != 64)
            burrow__fips140_record_non_approved();
    } else {
        burrow__fips140_record_non_approved();
    }
    arena_free(&scratch);

    return burrow__tls_prf12(a, hash, pre_master_secret,
                             BURROW_S("extended master secret"), transcript,
                             TLS_MASTER_SECRET_LENGTH);
}

CRYPTO_FIPS_TEXT Slice burrow__tls_prf(Alloc *a, TlsPrfKind prf_kind, Slice secret,
                                       Str label, Slice seed, Int key_len) {
    switch (prf_kind) {
    case TLS_PRF_10:
        return burrow__tls_prf10(a, secret, label, seed, key_len);
    case TLS_PRF_12_SHA384:
        return burrow__tls_prf12(a, sha512_new384, secret, label, seed, key_len);
    case TLS_PRF_12_SHA256:
    default:
        return burrow__tls_prf12(a, sha256_new, secret, label, seed, key_len);
    }
}

CRYPTO_FIPS_TEXT TlsPrfKind burrow__tls_prf_and_hash_for_version(uint16_t version,
                                                                 const TlsSuite *suite,
                                                                 CryptoHash *hash) {
    switch (version) {
    case TLS_VERSION_TLS10:
    case TLS_VERSION_TLS11:
        *hash = (CryptoHash)0;
        return TLS_PRF_10;
    case TLS_VERSION_TLS12:
        if ((suite->flags & TLS_SUITE_SHA384) != 0) {
            *hash = CRYPTO_SHA384;
            return TLS_PRF_12_SHA384;
        }
        *hash = CRYPTO_SHA256;
        return TLS_PRF_12_SHA256;
    default:
        panic_str(BURROW_S("unknown version"));
    }
}

CRYPTO_FIPS_TEXT static TlsPrfKind tls_prf_for_version(uint16_t version,
                                                       const TlsSuite *suite) {
    CryptoHash hash;
    return burrow__tls_prf_and_hash_for_version(version, suite, &hash);
}

/* a and then b, from scratch. */
CRYPTO_FIPS_TEXT static Slice tls_concat(Alloc *scratch, Slice a, Slice b) {
    Slice s = slice_make(scratch, TYPE_BYTE, 0, a.len + b.len);
    if (s.p == NULL && a.len + b.len > 0)
        return s;
    s = slice_append_slice(scratch, s, a);
    return slice_append_slice(scratch, s, b);
}

CRYPTO_FIPS_TEXT Slice burrow__tls_master_from_pre_master_secret(
    Alloc *a, uint16_t version, const TlsSuite *suite, Slice pre_master_secret,
    Slice client_random, Slice server_random) {
    TlsPrfKind prf = tls_prf_for_version(version, suite);
    Arena scratch;
    arena_init(&scratch, NULL, 0);
    Slice out = slice_nil(TYPE_BYTE);
    Slice seed = tls_concat(arena_allocator(&scratch), client_random, server_random);
    if (seed.p != NULL || seed.len == 0)
        out = burrow__tls_prf(a, prf, pre_master_secret, BURROW_S("master secret"),
                              seed, TLS_MASTER_SECRET_LENGTH);
    arena_free(&scratch);
    return out;
}

CRYPTO_FIPS_TEXT Slice burrow__tls_ext_master_from_pre_master_secret(
    Alloc *a, uint16_t version, const TlsSuite *suite, Slice pre_master_secret,
    Slice transcript) {
    CryptoHash hash;
    TlsPrfKind prf = burrow__tls_prf_and_hash_for_version(version, suite, &hash);
    if (version == TLS_VERSION_TLS12) {
        /* Use the FIPS 140-3 module only for TLS 1.2 with EMS, which is the
         * only TLS 1.0-1.2 approved mode per IG D.Q. */
        return burrow__tls12_master_secret(a, burrow__crypto_hash_new_func(hash),
                                           pre_master_secret, transcript);
    }
    return burrow__tls_prf(a, prf, pre_master_secret,
                           BURROW_S("extended master secret"), transcript,
                           TLS_MASTER_SECRET_LENGTH);
}

CRYPTO_FIPS_TEXT bool
burrow__tls_keys_from_master_secret(Alloc *a, uint16_t version, const TlsSuite *suite,
                                    Slice master_secret, Slice client_random,
                                    Slice server_random, Int mac_len, Int key_len,
                                    Int iv_len, TlsKeys *keys) {
    TlsPrfKind prf = tls_prf_for_version(version, suite);
    Arena scratch;
    arena_init(&scratch, NULL, 0);
    Int n = 2 * mac_len + 2 * key_len + 2 * iv_len;
    Slice km = slice_nil(TYPE_BYTE);
    Slice seed = tls_concat(arena_allocator(&scratch), server_random, client_random);
    if (seed.p != NULL || seed.len == 0)
        km = burrow__tls_prf(a, prf, master_secret, BURROW_S("key expansion"), seed, n);
    arena_free(&scratch);
    if (km.p == NULL && n > 0)
        return false;
    keys->client_mac = slice_sub(km, 0, mac_len);
    km = slice_sub(km, mac_len, km.len);
    keys->server_mac = slice_sub(km, 0, mac_len);
    km = slice_sub(km, mac_len, km.len);
    keys->client_key = slice_sub(km, 0, key_len);
    km = slice_sub(km, key_len, km.len);
    keys->server_key = slice_sub(km, 0, key_len);
    km = slice_sub(km, key_len, km.len);
    keys->client_iv = slice_sub(km, 0, iv_len);
    km = slice_sub(km, iv_len, km.len);
    keys->server_iv = slice_sub(km, 0, iv_len);
    return true;
}

/* ------------------------------------------------------------ finishedHash */

CRYPTO_FIPS_TEXT bool burrow__tls_new_finished_hash(Alloc *a, uint16_t version,
                                                    const TlsSuite *suite,
                                                    TlsFinishedHash *h) {
    memset(h, 0, sizeof *h);
    h->buffer = slice_nil(TYPE_BYTE);
    if (version >= TLS_VERSION_TLS12)
        h->buffer = slice_from(h, 0, 0, TYPE_BYTE); /* []byte{}, which is not nil */
    CryptoHash hash;
    h->prf = burrow__tls_prf_and_hash_for_version(version, suite, &hash);
    h->version = version;
    h->a = a;
    if (hash != 0) {
        HashNewFunc nf = burrow__crypto_hash_new_func(hash);
        h->client = nf(a);
        h->server = nf(a);
        return h->client.vt != NULL && h->server.vt != NULL;
    }
    h->client = sha1_new(a);
    h->server = sha1_new(a);
    h->client_md5 = md5_new(a);
    h->server_md5 = md5_new(a);
    return h->client.vt != NULL && h->server.vt != NULL && h->client_md5.vt != NULL &&
           h->server_md5.vt != NULL;
}

CRYPTO_FIPS_TEXT bool burrow__tls_finished_hash_write(TlsFinishedHash *h, Slice msg) {
    Error err = BURROW_NO_ERROR;
    hash_write(h->client, msg, &err);
    hash_write(h->server, msg, &err);

    if (h->version < TLS_VERSION_TLS12) {
        hash_write(h->client_md5, msg, &err);
        hash_write(h->server_md5, msg, &err);
    }

    if (!slice_is_nil(h->buffer)) {
        Slice b = slice_append_slice(h->a, h->buffer, msg);
        if (b.p == NULL && msg.len > 0) {
            h->oom = true;
            return false;
        }
        h->buffer = b;
    }
    return true;
}

CRYPTO_FIPS_TEXT static Int tls_finished_hash_io_write(void *self, Slice p,
                                                       Error *err) {
    if (!burrow__tls_finished_hash_write((TlsFinishedHash *)self, p)) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return 0;
    }
    return p.len;
}

static const IoWriterVT tls_finished_hash_writer_vt = {NULL,
                                                       tls_finished_hash_io_write};

CRYPTO_FIPS_TEXT IoWriter burrow__tls_finished_hash_writer(TlsFinishedHash *h) {
    return (IoWriter){&tls_finished_hash_writer_vt, h};
}

CRYPTO_FIPS_TEXT Slice burrow__tls_finished_hash_sum(const TlsFinishedHash *h,
                                                     Alloc *a) {
    if (h->version >= TLS_VERSION_TLS12)
        return hash_sum(a, h->client, slice_nil(TYPE_BYTE));

    Slice out = slice_make(a, TYPE_BYTE, 0, MD5_SIZE + SHA1_SIZE);
    if (out.p == NULL)
        return out;
    out = hash_sum(a, h->client_md5, out);
    return hash_sum(a, h->client, out);
}

/* The Finished verify_data for label. */
CRYPTO_FIPS_TEXT static Slice tls_finished_sum(const TlsFinishedHash *h, Alloc *a,
                                               Slice master_secret, Str label) {
    Arena scratch;
    arena_init(&scratch, NULL, 0);
    Slice out = slice_nil(TYPE_BYTE);
    Slice sum = burrow__tls_finished_hash_sum(h, arena_allocator(&scratch));
    if (sum.p != NULL)
        out = burrow__tls_prf(a, h->prf, master_secret, label, sum,
                              TLS_FINISHED_VERIFY_LENGTH);
    arena_free(&scratch);
    return out;
}

CRYPTO_FIPS_TEXT Slice burrow__tls_finished_hash_client_sum(const TlsFinishedHash *h,
                                                            Alloc *a,
                                                            Slice master_secret) {
    return tls_finished_sum(h, a, master_secret, BURROW_S("client finished"));
}

CRYPTO_FIPS_TEXT Slice burrow__tls_finished_hash_server_sum(const TlsFinishedHash *h,
                                                            Alloc *a,
                                                            Slice master_secret) {
    return tls_finished_sum(h, a, master_secret, BURROW_S("server finished"));
}

CRYPTO_FIPS_TEXT Slice burrow__tls_finished_hash_for_client_certificate(
    const TlsFinishedHash *h, Alloc *a, uint8_t sig_type) {
    if (sig_type == TLS_SIGNATURE_ECDSA)
        return hash_sum(a, h->server, slice_nil(TYPE_BYTE));
    return burrow__tls_finished_hash_sum(h, a);
}

CRYPTO_FIPS_TEXT void
burrow__tls_finished_hash_discard_handshake_buffer(TlsFinishedHash *h) {
    h->buffer = slice_nil(TYPE_BYTE);
}

/* --------------------------------------------------------------------- EKM */

BURROW_SENTINEL_ERROR(burrow__tls_err_no_ekm_renegotiation,
                      "crypto/tls: ExportKeyingMaterial is unavailable when "
                      "renegotiation is enabled");
BURROW_SENTINEL_ERROR(burrow__tls_err_no_ekm_no_ems,
                      "crypto/tls: ExportKeyingMaterial is unavailable when neither "
                      "TLS 1.3 nor Extended Master Secret are negotiated");

CRYPTO_FIPS_TEXT Slice burrow__tls_ekm_from_master_secret(const TlsEkmMaster *m,
                                                          Alloc *a, Str label,
                                                          Slice context, Int length,
                                                          Error *err) {
    if (str_eq(label, BURROW_S("client finished")) ||
        str_eq(label, BURROW_S("server finished")) ||
        str_eq(label, BURROW_S("master secret")) ||
        str_eq(label, BURROW_S("key expansion"))) {
        /* These values are reserved and may not be used. */
        BURROW_OUT(
            err,
            fmt_errorf_v("crypto/tls: reserved ExportKeyingMaterial label: %s", label));
        return slice_nil(TYPE_BYTE);
    }

    bool has_context = !slice_is_nil(context);
    if (has_context && context.len >= 1 << 16) {
        BURROW_OUT(err, errors_new(error_allocator(),
                                   BURROW_S("crypto/tls: ExportKeyingMaterial context "
                                            "too long")));
        return slice_nil(TYPE_BYTE);
    }

    Arena scratch;
    arena_init(&scratch, NULL, 0);
    Alloc *s = arena_allocator(&scratch);
    Int seed_len = m->server_random.len + m->client_random.len;
    if (has_context)
        seed_len += 2 + context.len;
    Slice out = slice_nil(TYPE_BYTE);
    Byte *seed = (Byte *)mem_alloc_nozero(s, (size_t)seed_len + 1, 1);
    if (seed != NULL) {
        Int n = 0;
        if (m->client_random.len > 0)
            memcpy(seed, m->client_random.p, (size_t)m->client_random.len);
        n += m->client_random.len;
        if (m->server_random.len > 0)
            memcpy(seed + n, m->server_random.p, (size_t)m->server_random.len);
        n += m->server_random.len;
        if (has_context) {
            seed[n++] = (Byte)(context.len >> 8);
            seed[n++] = (Byte)context.len;
            if (context.len > 0)
                memcpy(seed + n, context.p, (size_t)context.len);
        }
        out = burrow__tls_prf(a, tls_prf_for_version(m->version, m->suite),
                              m->master_secret, label,
                              slice_from(seed, seed_len, seed_len, TYPE_BYTE), length);
    }
    arena_free(&scratch);
    if (out.p == NULL && length > 0)
        BURROW_OUT(err, burrow_err_out_of_memory);
    return out;
}

CRYPTO_FIPS_TEXT static Slice tls_ekm_call(void *env, Alloc *a, Str label,
                                           Slice context, Int length, Error *err) {
    return burrow__tls_ekm_from_master_secret((const TlsEkmMaster *)env, a, label,
                                              context, length, err);
}

CRYPTO_FIPS_TEXT TlsEkmFunc burrow__tls_ekm_func(Alloc *a, const TlsEkmMaster *m) {
    TlsEkmFunc f = {0};
    TlsEkmMaster *c = BURROW_NEW(a, TlsEkmMaster);
    if (c == NULL)
        return f;
    c->version = m->version;
    c->suite = m->suite;
    c->master_secret = bytes_clone(a, m->master_secret);
    c->client_random = bytes_clone(a, m->client_random);
    c->server_random = bytes_clone(a, m->server_random);
    if ((c->master_secret.p == NULL && m->master_secret.len > 0) ||
        (c->client_random.p == NULL && m->client_random.len > 0) ||
        (c->server_random.p == NULL && m->server_random.len > 0))
        return f;
    return BURROW_FN(TlsEkmFunc, tls_ekm_call, c);
}

/* ------------------------------------------------------------------- CAST */

/* From Go's src/crypto/internal/fips140/tls12/cast.go. Go calls
 * tls12.MasterSecret, which is the PRF that
 * burrow__tls_ext_master_from_pre_master_secret runs for TLS 1.2. */

CRYPTO_FIPS_RODATA static const Byte tls12_cast_input[16] = {
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
    0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10,
};

CRYPTO_FIPS_RODATA static const Byte tls12_cast_transcript[16] = {
    0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
    0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20,
};

CRYPTO_FIPS_RODATA static const Byte tls12_cast_want[48] = {
    0x8c, 0x3e, 0xed, 0xa7, 0x1c, 0x1b, 0x4c, 0xc0, 0xa0, 0x44, 0x90, 0x75,
    0xa8, 0x8e, 0xbc, 0x7c, 0x5e, 0x1c, 0x4b, 0x1e, 0x4f, 0xe3, 0xc1, 0x06,
    0xeb, 0xdc, 0xc0, 0x5d, 0xc0, 0xc8, 0xec, 0xf3, 0xe2, 0xb9, 0xd1, 0x03,
    0x5e, 0xb2, 0x60, 0x5d, 0x12, 0x68, 0x4f, 0x49, 0xdf, 0xa9, 0x9d, 0xcc,
};

CRYPTO_FIPS_TEXT Error burrow__tls12_cast(void *arg) {
    (void)arg;
    Slice input = slice_from((void *)(uintptr_t)tls12_cast_input, 16, 16, TYPE_BYTE);
    Slice transcript =
        slice_from((void *)(uintptr_t)tls12_cast_transcript, 16, 16, TYPE_BYTE);
    Arena ar;
    arena_init(&ar, NULL, 0);
    Slice got = burrow__tls12_master_secret(arena_allocator(&ar), sha256_new, input,
                                            transcript);
    if (got.p == NULL) {
        arena_free(&ar);
        return burrow_err_out_of_memory;
    }
    bool ok = got.len == (Int)sizeof tls12_cast_want &&
              memcmp(got.p, tls12_cast_want, sizeof tls12_cast_want) == 0;
    arena_free(&ar);
    return ok ? BURROW_NO_ERROR
              : errors_new(error_allocator(), BURROW_S("unexpected result"));
}
