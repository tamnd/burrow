/* Derived from Go's src/crypto/internal/fips140/ssh/kdf.go.
 * Go source: go1.27.1.
 *
 * Copyright 2024 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "ssh_internal.h"

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/hash.h"
#include "burrow/mem.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include "fips140_module.h"

CRYPTO_FIPS_RODATA const SshDirection burrow__ssh_server_keys = {'B', 'D', 'F'};
CRYPTO_FIPS_RODATA const SshDirection burrow__ssh_client_keys = {'A', 'C', 'E'};

/* generateKeyMaterial. Not in Go: key is made with room for every round up
 * front, so that each h.Sum(key) appends in place rather than leaving the
 * shorter arrays behind in a. False when a runs out. */
CRYPTO_FIPS_TEXT static bool ssh_generate_key_material(Alloc *a, Hash hh, Slice k,
                                                       Slice h, Byte tag,
                                                       Slice session_id, Int length,
                                                       Slice *out) {
    Int size = hash_size(hh);
    Int rounds = (length + size - 1) / size;
    Slice key = slice_make(a, TYPE_BYTE, 0, rounds * size);
    if (key.p == NULL && rounds > 0)
        return false;
    while (key.len < length) {
        hash_reset(hh);
        hash_write(hh, k, NULL);
        hash_write(hh, h, NULL);
        if (key.len == 0) {
            hash_write(hh, slice_from(&tag, 1, 1, TYPE_BYTE), NULL);
            hash_write(hh, session_id, NULL);
        } else {
            hash_write(hh, key, NULL);
        }
        key = hash_sum(a, hh, key);
        if (key.p == NULL)
            return false;
    }
    *out = slice_sub(key, 0, length);
    return true;
}

CRYPTO_FIPS_TEXT void burrow__ssh_keys(Alloc *a, HashNewFunc hash, SshDirection d,
                                       Slice k, Slice h, Slice session_id,
                                       Int iv_key_len, Int key_len, Int mac_key_len,
                                       Slice *iv_key, Slice *key, Slice *mac_key,
                                       Error *err) {
    *iv_key = slice_nil(TYPE_BYTE);
    *key = slice_nil(TYPE_BYTE);
    *mac_key = slice_nil(TYPE_BYTE);
    Hash hh = hash(a);
    Slice iv = slice_nil(TYPE_BYTE), enc = slice_nil(TYPE_BYTE),
          mac = slice_nil(TYPE_BYTE);
    if (hh.vt == NULL ||
        !ssh_generate_key_material(a, hh, k, h, d.iv_tag, session_id, iv_key_len,
                                   &iv) ||
        !ssh_generate_key_material(a, hh, k, h, d.key_tag, session_id, key_len, &enc) ||
        !ssh_generate_key_material(a, hh, k, h, d.mac_key_tag, session_id, mac_key_len,
                                   &mac)) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return;
    }
    *iv_key = iv;
    *key = enc;
    *mac_key = mac;
    BURROW_OUT(err, BURROW_NO_ERROR);
}
