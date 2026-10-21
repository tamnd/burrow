/* Derived from Go's src/crypto/hkdf/hkdf.go and
 * src/crypto/internal/fips140/hkdf/hkdf.go.
 * Go source: go1.27.1.
 *
 * Copyright 2014 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/crypto/hkdf.h"

#include "burrow/core.h"
#include "burrow/crypto/hmac.h"
#include "burrow/crypto/sha256.h"
#include "burrow/error.h"
#include "burrow/hash.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include "fips140_module.h"
#include "fips140only_internal.h"

#include <stdint.h>
#include <string.h>

CRYPTO_FIPS_TEXT Slice burrow__hkdf_extract(Alloc *a, HashNewFunc h, Slice secret,
                                            Slice salt, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    if (secret.len < 112 / 8)
        burrow__fips140_record_non_approved();
    /* A missing salt is a block of zeros as long as the sum, and HMAC pads
     * an empty key with zeros to the block size anyway, so an empty salt
     * gives the same answer without making the zeros. */
    Hash ext = burrow__hmac_new(a, h, salt);
    if (ext.vt == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return slice_nil(TYPE_BYTE);
    }
    burrow__hmac_mark_as_used_in_kdf(ext);
    hash_write(ext, secret, NULL);
    return hash_sum(a, ext, slice_nil(TYPE_BYTE));
}

CRYPTO_FIPS_TEXT Slice burrow__hkdf_expand(Alloc *a, HashNewFunc h, Slice prk, Str info,
                                           Int key_length, Error *err) {
    Slice out = slice_make(a, TYPE_BYTE, 0, key_length);
    Hash exp = burrow__hmac_new(a, h, prk);
    if (exp.vt == NULL || (out.p == NULL && key_length > 0)) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return slice_nil(TYPE_BYTE);
    }
    burrow__hmac_mark_as_used_in_kdf(exp);
    Slice inf = slice_from((void *)(uintptr_t)info.p, info.len, info.len, TYPE_BYTE);
    Byte counter = 0;
    Slice buf = slice_nil(TYPE_BYTE);
    while (out.len < key_length) {
        counter++;
        if (counter == 0)
            panic_str(BURROW_S("hkdf: counter overflow"));
        if (counter > 1)
            hash_reset(exp);
        hash_write(exp, buf, NULL);
        hash_write(exp, inf, NULL);
        hash_write(exp, slice_from(&counter, 1, 1, TYPE_BYTE), NULL);
        buf = hash_sum(a, exp, slice_sub(buf, 0, 0));
        Int remain = key_length - out.len;
        if (remain > buf.len)
            remain = buf.len;
        out = slice_append(a, out, buf.p, remain);
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return out;
}

BURROW_FIPS140ONLY_ERROR(
    hkdf_err_short_key_fips140only,
    "crypto/hkdf: use of keys shorter than 112 bits is not allowed "
    "in FIPS 140-only mode");
BURROW_FIPS140ONLY_ERROR(hkdf_err_hash_fips140only,
                         "crypto/hkdf: use of hash functions other than SHA-2 or SHA-3 "
                         "is not allowed in FIPS 140-only mode");

CRYPTO_FIPS_TEXT static bool hkdf_check_fips140_only(HashNewFunc h, Slice key,
                                                     Error *err) {
    if (!burrow__fips140only_enforced())
        return true;
    if (key.len < 112 / 8) {
        BURROW_OUT(err, hkdf_err_short_key_fips140only);
        return false;
    }
    if (!burrow__fips140only_approved_hash_func(h)) {
        BURROW_OUT(err, hkdf_err_hash_fips140only);
        return false;
    }
    return true;
}

CRYPTO_FIPS_TEXT Slice hkdf_extract(Alloc *a, HashNewFunc h, Slice secret, Slice salt,
                                    Error *err) {
    if (!hkdf_check_fips140_only(h, secret, err))
        return slice_nil(TYPE_BYTE);
    return burrow__hkdf_extract(a, h, secret, salt, err);
}

/* Go checks the length against the hash's size in the exported functions,
 * before the FIPS module that does the work ever sees it. */
CRYPTO_FIPS_TEXT static bool hkdf_too_long(Alloc *a, HashNewFunc h, Int key_length,
                                           Error *err) {
    Hash probe = h(a);
    if (probe.vt == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return true;
    }
    if (key_length > hash_size(probe) * 255) {
        BURROW_OUT(err, errors_new(error_allocator(),
                                   BURROW_S("hkdf: requested key length too large")));
        return true;
    }
    return false;
}

CRYPTO_FIPS_TEXT Slice hkdf_expand(Alloc *a, HashNewFunc h, Slice pseudorandom_key,
                                   Str info, Int key_length, Error *err) {
    if (!hkdf_check_fips140_only(h, pseudorandom_key, err))
        return slice_nil(TYPE_BYTE);
    if (hkdf_too_long(a, h, key_length, err))
        return slice_nil(TYPE_BYTE);
    return burrow__hkdf_expand(a, h, pseudorandom_key, info, key_length, err);
}

/* The module's hkdf.Key. */
CRYPTO_FIPS_TEXT static Slice hkdf_module_key(Alloc *a, HashNewFunc h, Slice secret,
                                              Slice salt, Str info, Int key_length,
                                              Error *err) {
    Slice prk = burrow__hkdf_extract(a, h, secret, salt, err);
    if (prk.p == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return slice_nil(TYPE_BYTE);
    }
    return burrow__hkdf_expand(a, h, prk, info, key_length, err);
}

CRYPTO_FIPS_TEXT Slice hkdf_key(Alloc *a, HashNewFunc h, Slice secret, Slice salt,
                                Str info, Int key_length, Error *err) {
    if (!hkdf_check_fips140_only(h, secret, err))
        return slice_nil(TYPE_BYTE);
    if (hkdf_too_long(a, h, key_length, err))
        return slice_nil(TYPE_BYTE);
    return hkdf_module_key(a, h, secret, salt, info, key_length, err);
}

/* ------------------------------------------------------------------- CAST */

/* From Go's src/crypto/internal/fips140/hkdf/cast.go. */

CRYPTO_FIPS_RODATA static const Byte hkdf_cast_input[16] = {
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
    0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10,
};

CRYPTO_FIPS_RODATA static const Byte hkdf_cast_want[32] = {
    0xb6, 0x53, 0x00, 0x5b, 0x51, 0x6d, 0x2b, 0xc9, 0x4a, 0xe4, 0xf9,
    0x51, 0x73, 0x1f, 0x71, 0x21, 0xa6, 0xc1, 0xde, 0x42, 0x4f, 0x2c,
    0x99, 0x60, 0x64, 0xdb, 0x66, 0x3e, 0xec, 0xa6, 0x37, 0xff,
};

CRYPTO_FIPS_TEXT Error burrow__hkdf_cast(void *arg) {
    (void)arg;
    Int n = (Int)sizeof hkdf_cast_input;
    Slice input = slice_from((void *)(uintptr_t)hkdf_cast_input, n, n, TYPE_BYTE);
    Str info = str_from_bytes(hkdf_cast_input, n);
    Arena ar;
    arena_init(&ar, NULL, 0);
    Error err = BURROW_NO_ERROR;
    Slice got = hkdf_module_key(arena_allocator(&ar), sha256_new, input, input, info,
                                (Int)sizeof hkdf_cast_want, &err);
    if (BURROW_FAILED(err)) {
        arena_free(&ar);
        return err;
    }
    bool ok = got.len == (Int)sizeof hkdf_cast_want &&
              memcmp(got.p, hkdf_cast_want, sizeof hkdf_cast_want) == 0;
    arena_free(&ar);
    return ok ? BURROW_NO_ERROR
              : errors_new(error_allocator(), BURROW_S("unexpected result"));
}
