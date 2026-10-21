/* crypto/internal/fips140/ssh's KDF, from ssh_kdf.c.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_SRC_CRYPTO_SSH_INTERNAL_H
#define BURROW_SRC_CRYPTO_SSH_INTERNAL_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/hash.h"
#include "burrow/mem.h"
#include "burrow/slice.h"

/* Package ssh implements the SSH KDF as specified in RFC 4253,
 * Section 7.2 and allowed by SP 800-135 Revision 1. */

/* Direction: the letters RFC 4253 hashes in for one side's initial IV,
 * encryption key and integrity key. Not in Go: each is a Byte rather than a
 * []byte of one. */
typedef struct SshDirection {
    Byte iv_tag;
    Byte key_tag;
    Byte mac_key_tag;
} SshDirection;

/* ServerKeys, the keys for server to client, and ClientKeys, for client to
 * server. */
extern const SshDirection burrow__ssh_server_keys;
extern const SshDirection burrow__ssh_client_keys;

/* Keys: the initial IV, the encryption key and the integrity key for direction
 * d, ivKeyLen, keyLen and macKeyLen bytes long, from the shared secret k, the
 * exchange hash h and the session identifier.
 *
 * Not in Go: the three keys come back through iv_key, key and mac_key, made
 * from a, as is the hash. When a runs out, err is burrow_err_out_of_memory and
 * all three are nil. */
void burrow__ssh_keys(Alloc *a, HashNewFunc hash, SshDirection d, Slice k, Slice h,
                      Slice session_id, Int iv_key_len, Int key_len, Int mac_key_len,
                      Slice *iv_key, Slice *key, Slice *mac_key, Error *err);

#endif /* BURROW_SRC_CRYPTO_SSH_INTERNAL_H */
