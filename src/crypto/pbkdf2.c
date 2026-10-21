/* Derived from Go's src/crypto/pbkdf2/pbkdf2.go and
 * src/crypto/internal/fips140/pbkdf2/pbkdf2.go.
 * Go source: go1.27.1.
 *
 * Copyright 2012 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/crypto/pbkdf2.h"

#include "burrow/core.h"
#include "burrow/crypto/hmac.h"
#include "burrow/crypto/sha256.h"
#include "burrow/error.h"
#include "burrow/hash.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include "fips140_module.h"

#include <stdint.h>
#include <string.h>

CRYPTO_FIPS_TEXT Slice pbkdf2_key(Alloc *a, HashNewFunc h, Str password, Slice salt,
                                  Int iter, Int key_length, Error *err) {
    Slice nil = slice_nil(TYPE_BYTE);
    if (key_length <= 0) {
        BURROW_OUT(err,
                   errors_new(error_allocator(),
                              BURROW_S("pbkdf2: keyLength must be larger than 0")));
        return nil;
    }
    Hash prf = hmac_new(a, h,
                        slice_from((void *)(uintptr_t)password.p, password.len,
                                   password.len, TYPE_BYTE));
    if (prf.vt == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return nil;
    }
    Int hash_len = hash_size(prf);
    /* Go tests keyLength+hashLen < keyLength, which relies on the sum wrapping.
     * Here the same limit is tested before anything is added, and only then is
     * the block count worked out. */
    int64_t num_blocks = 0;
    if (key_length <= BURROW_INT_MAX - hash_len)
        num_blocks = ((int64_t)key_length + hash_len - 1) / hash_len;
    if (num_blocks == 0 || num_blocks > (int64_t)UINT32_MAX) {
        BURROW_OUT(
            err, errors_new(error_allocator(), BURROW_S("pbkdf2: keyLength too long")));
        return nil;
    }

    Slice dk = slice_make(a, TYPE_BYTE, 0, (Int)num_blocks * hash_len);
    Slice u = slice_make(a, TYPE_BYTE, hash_len, hash_len);
    if (dk.p == NULL || u.p == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return nil;
    }
    Byte buf[4];
    for (int64_t block = 1; block <= num_blocks; block++) {
        hash_reset(prf);
        hash_write(prf, salt, NULL);
        buf[0] = (Byte)(block >> 24);
        buf[1] = (Byte)(block >> 16);
        buf[2] = (Byte)(block >> 8);
        buf[3] = (Byte)block;
        hash_write(prf, slice_from(buf, 4, 4, TYPE_BYTE), NULL);
        dk = hash_sum(a, prf, dk);
        Byte *t = (Byte *)dk.p + dk.len - hash_len;
        memcpy(u.p, t, (size_t)hash_len);
        for (Int n = 2; n <= iter; n++) {
            hash_reset(prf);
            hash_write(prf, u, NULL);
            u = hash_sum(a, prf, slice_sub(u, 0, 0));
            const Byte *up = (const Byte *)u.p;
            for (Int x = 0; x < hash_len; x++)
                t[x] ^= up[x];
        }
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return slice_sub(dk, 0, key_length);
}

/* ------------------------------------------------------------------- CAST */

/* From Go's src/crypto/internal/fips140/pbkdf2/cast.go.
 *
 * Per IG 10.3.A:
 *   "if the module implements an approved PBKDF (SP 800-132), the module
 *    shall perform a CAST, at minimum, on the derivation of the Master
 *   Key (MK) as specified in Section 5.3 of SP 800-132"
 *   "The Iteration Count parameter does not need to be among those
 *   supported by the module in the approved mode but shall be at least
 *   two." */

CRYPTO_FIPS_RODATA static const Byte pbkdf2_cast_salt[16] = {
    0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11,
    0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19,
};

CRYPTO_FIPS_RODATA static const Byte pbkdf2_cast_want[14] = {
    0xC7, 0x58, 0x76, 0xC0, 0x71, 0x1C, 0x29, 0x75, 0x2D, 0x3A, 0xA6, 0xDF, 0x29, 0x96,
};

CRYPTO_FIPS_TEXT Error burrow__pbkdf2_cast(void *arg) {
    Int n = (Int)sizeof pbkdf2_cast_salt;
    Slice salt = slice_from((void *)(uintptr_t)pbkdf2_cast_salt, n, n, TYPE_BYTE);
    Arena ar;
    arena_init(&ar, NULL, 0);
    Error err = BURROW_NO_ERROR;
    Slice mk = pbkdf2_key(arena_allocator(&ar), sha256_new, BURROW_S("password"), salt,
                          2, 14, &err);
    if (BURROW_FAILED(err)) {
        arena_free(&ar);
        return err;
    }
    bool ok = mk.len == (Int)sizeof pbkdf2_cast_want &&
              memcmp(mk.p, pbkdf2_cast_want, sizeof pbkdf2_cast_want) == 0;
    arena_free(&ar);
    return ok ? BURROW_NO_ERROR
              : errors_new(error_allocator(), BURROW_S("unexpected result"));
}
