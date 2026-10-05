/* What crypto/hpke's tests reach into. Go's tests set two package variables,
 * testingOnlyGenerateKey and testingOnlyEncapsulate, to make an encapsulation
 * come out the way a test vector says. burrow has no mutable globals for that,
 * so the same randomness is an argument of a sender made for the purpose.
 *
 * Copyright 2024 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_SRC_CRYPTO_HPKE_INTERNAL_H
#define BURROW_SRC_CRYPTO_HPKE_INTERNAL_H

#include "burrow/crypto/hpke.h"

#include "burrow/core.h"
#include "burrow/crypto/ecdh.h"
#include "burrow/error.h"
#include "burrow/mem.h"
#include "burrow/slice.h"

#include <stdbool.h>

/* The randomness of one encapsulation. ephemeral, when it is not NULL, is the
 * ephemeral key of a DHKEM or of the curve half of a hybrid, in place of a
 * generated one. When fixed_pq is set, pq_shared_key and pq_ciphertext are
 * what the ML-KEM encapsulation of ML-KEM or of a hybrid gives. */
typedef struct HpkeEncapRandomness {
    const EcdhPrivateKey *ephemeral;
    bool fixed_pq;
    Slice pq_shared_key, pq_ciphertext;
} HpkeEncapRandomness;

/* hpke_new_sender with the randomness r, which can be NULL for none. */
BURROW_OWNS(ret) Slice burrow__hpke_new_sender_with(Alloc *a, const HpkePublicKey *pk,
                                                    const HpkeKDF *kdf,
                                                    const HpkeAEAD *aead, Slice info,
                                                    const HpkeEncapRandomness *r,
                                                    HpkeSender **s, Error *err);

/* The size of what the KEM's encapsulation gives, Go's KEM.encSize. */
Int burrow__hpke_kem_enc_size(const HpkeKEM *kem);

#endif /* BURROW_SRC_CRYPTO_HPKE_INTERNAL_H */
