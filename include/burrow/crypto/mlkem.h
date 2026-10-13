/* crypto/mlkem, the quantum-resistant key encapsulation method ML-KEM, once
 * called Kyber, of NIST FIPS 203.
 *
 *     // Alice makes a key and sends Bob the encapsulation key.
 *     MlkemDecapsulationKey768 *dk = mlkem_generate_key768(a, &err);
 *     Slice ek_bytes = mlkem_encapsulation_key768_bytes(
 *         mlkem_decapsulation_key768_encapsulation_key(dk), a);
 *
 *     // Bob makes a shared key and the ciphertext that carries it.
 *     MlkemEncapsulationKey768 *ek = mlkem_new_encapsulation_key768(a, ek_bytes, &err);
 *     Slice ciphertext;
 *     Slice bob_key = mlkem_encapsulation_key768_encapsulate(ek, a, &ciphertext);
 *
 *     // Alice gets the same shared key out of the ciphertext.
 *     Slice alice_key = mlkem_decapsulation_key768_decapsulate(dk, a, ciphertext, &err);
 *
 * Most programs want ML-KEM-768. ML-KEM-1024 is the same with bigger keys and
 * ciphertexts, and ML-KEM-512 is not here, as it is not in Go.
 *
 * Keys are allocated from the Alloc they are made with, in one block that holds
 * the expanded form along with the seed, and freed with the _free function of
 * their type. The encapsulation key of a decapsulation key is part of the same
 * block and is not freed on its own. A decapsulation key is about 8 KB for
 * ML-KEM-768 and 11 KB for ML-KEM-1024.
 *
 * Every operation on secret data is constant time. Go does the FIPS 140 self
 * test and the pairwise consistency test of a new key only in FIPS mode, which
 * burrow does not have, so it does neither.
 *
 * Copyright 2023 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package crypto/mlkem */

#ifndef BURROW_CRYPTO_MLKEM_H
#define BURROW_CRYPTO_MLKEM_H

#include "burrow/core.h"
#include "burrow/crypto.h"
#include "burrow/error.h"
#include "burrow/mem.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#ifdef __cplusplus
extern "C" {
#endif

/* SharedKeySize: the size of a shared key. */
#define MLKEM_SHARED_KEY_SIZE 32

/* SeedSize: the size of the seed a decapsulation key is made from. */
#define MLKEM_SEED_SIZE 64

/* CiphertextSize768 and EncapsulationKeySize768: the sizes of an ML-KEM-768
 * ciphertext and encapsulation key. */
#define MLKEM_CIPHERTEXT_SIZE768 1088
#define MLKEM_ENCAPSULATION_KEY_SIZE768 1184

/* CiphertextSize1024 and EncapsulationKeySize1024: the same for ML-KEM-1024. */
#define MLKEM_CIPHERTEXT_SIZE1024 1568
#define MLKEM_ENCAPSULATION_KEY_SIZE1024 1568

/* DecapsulationKey768: the secret key that gets a shared key out of a
 * ciphertext, with the values worked out from it. */
typedef struct MlkemDecapsulationKey768 MlkemDecapsulationKey768;

extern const Type burrow_type_MlkemDecapsulationKey768;
#define TYPE_MLKEM_DECAPSULATION_KEY768 TYPE_OF(MlkemDecapsulationKey768)

/* EncapsulationKey768: the public key that makes ciphertexts for the
 * MlkemDecapsulationKey768 it belongs to. */
typedef struct MlkemEncapsulationKey768 MlkemEncapsulationKey768;

extern const Type burrow_type_MlkemEncapsulationKey768;
#define TYPE_MLKEM_ENCAPSULATION_KEY768 TYPE_OF(MlkemEncapsulationKey768)

/* DecapsulationKey1024 and EncapsulationKey1024: the same for ML-KEM-1024. */
typedef struct MlkemDecapsulationKey1024 MlkemDecapsulationKey1024;

extern const Type burrow_type_MlkemDecapsulationKey1024;
#define TYPE_MLKEM_DECAPSULATION_KEY1024 TYPE_OF(MlkemDecapsulationKey1024)

typedef struct MlkemEncapsulationKey1024 MlkemEncapsulationKey1024;

extern const Type burrow_type_MlkemEncapsulationKey1024;
#define TYPE_MLKEM_ENCAPSULATION_KEY1024 TYPE_OF(MlkemEncapsulationKey1024)

/* ------------------------------------------------------------- ML-KEM-768 */

/* GenerateKey768: a new decapsulation key from the system's generator, from
 * a. The only error is running out of memory. Keep the key secret. */
BURROW_OWNS(ret) MlkemDecapsulationKey768 *mlkem_generate_key768(Alloc *a, Error *err);

/* NewDecapsulationKey768: the key for a 64 byte seed in the d || z form, from
 * a. The seed has to be uniformly random. Any other length is "mlkem: invalid
 * seed length". */
BURROW_OWNS(ret) MlkemDecapsulationKey768 *
mlkem_new_decapsulation_key768(Alloc *a, Slice seed, Error *err);

/* Frees a key from mlkem_generate_key768 or mlkem_new_decapsulation_key768,
 * clearing it first. NULL is fine. */
void mlkem_decapsulation_key768_free(MlkemDecapsulationKey768 *dk);

/* DecapsulationKey768.Bytes: the 64 byte seed of dk, d then z, from a. Keep it
 * secret. */
BURROW_OWNS(ret) Slice
mlkem_decapsulation_key768_bytes(const MlkemDecapsulationKey768 *dk, Alloc *a);

/* DecapsulationKey768.Decapsulate: the shared key in ciphertext, from a. A
 * ciphertext of the wrong length is "mlkem: invalid ciphertext length". One of
 * the right length that is not valid is not an error: it gives a shared key
 * that is not the sender's, as FIPS 203 says. Keep the shared key secret. */
BURROW_OWNS(ret) Slice mlkem_decapsulation_key768_decapsulate(
    const MlkemDecapsulationKey768 *dk, Alloc *a, Slice ciphertext, Error *err);

/* DecapsulationKey768.EncapsulationKey: the public key of dk, which is part of
 * dk. */
BURROW_BORROWS(ret, dk) const MlkemEncapsulationKey768 *
mlkem_decapsulation_key768_encapsulation_key(const MlkemDecapsulationKey768 *dk);

/* DecapsulationKey768.Encapsulator: the same, as a CryptoEncapsulator. */
BURROW_BORROWS(ret, dk) CryptoEncapsulator
mlkem_decapsulation_key768_encapsulator(const MlkemDecapsulationKey768 *dk);

/* dk as a CryptoDecapsulator, which borrows it. */
BURROW_BORROWS(ret, dk) CryptoDecapsulator
mlkem_decapsulation_key768_as_decapsulator(const MlkemDecapsulationKey768 *dk);

/* NewEncapsulationKey768: the key in its encoded form, from a. A key of the
 * wrong length is "mlkem: invalid encapsulation key length", and one with a
 * coefficient that is not reduced is "mlkem: invalid polynomial encoding". */
BURROW_OWNS(ret) MlkemEncapsulationKey768 *
mlkem_new_encapsulation_key768(Alloc *a, Slice encapsulation_key, Error *err);

/* Frees a key from mlkem_new_encapsulation_key768. NULL is fine, and so is the
 * key of a decapsulation key, which is left alone. */
void mlkem_encapsulation_key768_free(MlkemEncapsulationKey768 *ek);

/* EncapsulationKey768.Bytes: the encoded form of ek, from a. */
BURROW_OWNS(ret) Slice
mlkem_encapsulation_key768_bytes(const MlkemEncapsulationKey768 *ek, Alloc *a);

/* EncapsulationKey768.Encapsulate: a new shared key, returned, and the
 * ciphertext that carries it, in *ciphertext, both from a, with randomness from
 * the system's generator. Keep the shared key secret. If a runs out of memory
 * both are nil Slices. For a known answer test, crypto/mlkem/mlkemtest has the
 * same with the randomness given. */
BURROW_OWNS(ret) Slice mlkem_encapsulation_key768_encapsulate(
    const MlkemEncapsulationKey768 *ek, Alloc *a, Slice *ciphertext);

/* ek as a CryptoEncapsulator, which borrows it. */
BURROW_BORROWS(ret, ek) CryptoEncapsulator
mlkem_encapsulation_key768_as_encapsulator(const MlkemEncapsulationKey768 *ek);

/* ------------------------------------------------------------ ML-KEM-1024 */

/* The same as the functions above, for ML-KEM-1024. */

BURROW_OWNS(ret) MlkemDecapsulationKey1024 *mlkem_generate_key1024(Alloc *a,
                                                                   Error *err);

BURROW_OWNS(ret) MlkemDecapsulationKey1024 *
mlkem_new_decapsulation_key1024(Alloc *a, Slice seed, Error *err);

void mlkem_decapsulation_key1024_free(MlkemDecapsulationKey1024 *dk);

BURROW_OWNS(ret) Slice
mlkem_decapsulation_key1024_bytes(const MlkemDecapsulationKey1024 *dk, Alloc *a);

BURROW_OWNS(ret) Slice mlkem_decapsulation_key1024_decapsulate(
    const MlkemDecapsulationKey1024 *dk, Alloc *a, Slice ciphertext, Error *err);

BURROW_BORROWS(ret, dk) const MlkemEncapsulationKey1024 *
mlkem_decapsulation_key1024_encapsulation_key(const MlkemDecapsulationKey1024 *dk);

BURROW_BORROWS(ret, dk) CryptoEncapsulator
mlkem_decapsulation_key1024_encapsulator(const MlkemDecapsulationKey1024 *dk);

BURROW_BORROWS(ret, dk) CryptoDecapsulator
mlkem_decapsulation_key1024_as_decapsulator(const MlkemDecapsulationKey1024 *dk);

BURROW_OWNS(ret) MlkemEncapsulationKey1024 *
mlkem_new_encapsulation_key1024(Alloc *a, Slice encapsulation_key, Error *err);

void mlkem_encapsulation_key1024_free(MlkemEncapsulationKey1024 *ek);

BURROW_OWNS(ret) Slice
mlkem_encapsulation_key1024_bytes(const MlkemEncapsulationKey1024 *ek, Alloc *a);

BURROW_OWNS(ret) Slice mlkem_encapsulation_key1024_encapsulate(
    const MlkemEncapsulationKey1024 *ek, Alloc *a, Slice *ciphertext);

BURROW_BORROWS(ret, ek) CryptoEncapsulator
mlkem_encapsulation_key1024_as_encapsulator(const MlkemEncapsulationKey1024 *ek);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_CRYPTO_MLKEM_H */
