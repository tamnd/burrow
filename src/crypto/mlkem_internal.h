/* The parts of crypto/mlkem that its tests look at, from Go's
 * crypto/internal/fips140/mlkem: the field and ring arithmetic of field.go,
 * the derandomized key generation, and the expanded NIST encoding of a
 * decapsulation key that only the ACVP and Wycheproof tests use.
 *
 * Copyright 2024 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_SRC_CRYPTO_MLKEM_INTERNAL_H
#define BURROW_SRC_CRYPTO_MLKEM_INTERNAL_H

#include "burrow/crypto/mlkem.h"

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/mem.h"
#include "burrow/slice.h"

#include <stdint.h>

enum {
    MLKEM_N = 256,
    MLKEM_Q = 3329,

    /* The size of a ring element encoded with d bits a coefficient. */
    MLKEM_ENCODING_SIZE12 = MLKEM_N * 12 / 8,
    MLKEM_ENCODING_SIZE11 = MLKEM_N * 11 / 8,
    MLKEM_ENCODING_SIZE10 = MLKEM_N * 10 / 8,
    MLKEM_ENCODING_SIZE5 = MLKEM_N * 5 / 8,
    MLKEM_ENCODING_SIZE4 = MLKEM_N * 4 / 8,
    MLKEM_ENCODING_SIZE1 = MLKEM_N * 1 / 8,

    MLKEM_MESSAGE_SIZE = MLKEM_ENCODING_SIZE1,

    MLKEM_DECAPSULATION_KEY_SIZE768 =
        3 * MLKEM_ENCODING_SIZE12 + MLKEM_ENCAPSULATION_KEY_SIZE768 + 32 + 32,
    MLKEM_DECAPSULATION_KEY_SIZE1024 =
        4 * MLKEM_ENCODING_SIZE12 + MLKEM_ENCAPSULATION_KEY_SIZE1024 + 32 + 32,
};

/* fieldElement: an integer modulo q, always reduced. */
typedef uint16_t MlkemFieldElement;

/* ringElement and nttElement, which Go keeps as two types of the same array
 * and this keeps as one: a polynomial, or its NTT representation. */
typedef struct MlkemRing {
    MlkemFieldElement c[MLKEM_N];
} MlkemRing;

/* zetas and gammas, from FIPS 203, Appendix A. */
extern const MlkemFieldElement burrow__mlkem_zetas[128];
extern const MlkemFieldElement burrow__mlkem_gammas[128];

/* The field operations. a for reduce is less than 2q². */
MlkemFieldElement burrow__mlkem_field_reduce(uint32_t a);
MlkemFieldElement burrow__mlkem_field_add(MlkemFieldElement a, MlkemFieldElement b);
MlkemFieldElement burrow__mlkem_field_sub(MlkemFieldElement a, MlkemFieldElement b);
MlkemFieldElement burrow__mlkem_field_mul(MlkemFieldElement a, MlkemFieldElement b);

/* Compress and Decompress from FIPS 203, Definitions 4.7 and 4.8, for d from
 * 1 to 11. */
uint16_t burrow__mlkem_compress(MlkemFieldElement x, uint8_t d);
MlkemFieldElement burrow__mlkem_decompress(uint16_t y, uint8_t d);

/* Compress with d bits then ByteEncode, into the 32 * d bytes at out, and the
 * other way from the 32 * d bytes at b. The ones with a number are the same
 * for that d, written out. */
void burrow__mlkem_ring_compress_and_encode(Byte *out, const MlkemRing *f, uint8_t d);
void burrow__mlkem_ring_compress_and_encode1(Byte *out, const MlkemRing *f);
void burrow__mlkem_ring_compress_and_encode4(Byte *out, const MlkemRing *f);
void burrow__mlkem_ring_compress_and_encode10(Byte *out, const MlkemRing *f);
void burrow__mlkem_ring_decode_and_decompress(MlkemRing *f, const Byte *b, uint8_t d);
void burrow__mlkem_ring_decode_and_decompress1(MlkemRing *f, const Byte *b);
void burrow__mlkem_ring_decode_and_decompress4(MlkemRing *f, const Byte *b);
void burrow__mlkem_ring_decode_and_decompress10(MlkemRing *f, const Byte *b);

/* GenerateKeyInternal768 and 1024: the key for the seed d || z, as
 * mlkem_new_decapsulation_key768 would make it, from a. NULL when a is out of
 * memory. */
BURROW_OWNS(ret) MlkemDecapsulationKey768 *
burrow__mlkem_generate_key_internal768(Alloc *a, const Byte d[32], const Byte z[32]);
BURROW_OWNS(ret) MlkemDecapsulationKey1024 *
burrow__mlkem_generate_key_internal1024(Alloc *a, const Byte d[32], const Byte z[32]);

/* TestingOnlyExpandedBytes768 and 1024: dk in the expanded NIST encoding,
 * s, then the encapsulation key, then H(ek) and z, from a. */
BURROW_OWNS(ret) Slice burrow__mlkem_testing_only_expanded_bytes768(
    const MlkemDecapsulationKey768 *dk, Alloc *a);
BURROW_OWNS(ret) Slice burrow__mlkem_testing_only_expanded_bytes1024(
    const MlkemDecapsulationKey1024 *dk, Alloc *a);

/* TestingOnlyNewDecapsulationKey768 and 1024: a key from its expanded NIST
 * encoding. d is random, so mlkem_decapsulation_key768_bytes of it is not
 * the seed of anything. */
BURROW_OWNS(ret) MlkemDecapsulationKey768 *
burrow__mlkem_testing_only_new_decapsulation_key768(Alloc *a, Slice b, Error *err);
BURROW_OWNS(ret) MlkemDecapsulationKey1024 *
burrow__mlkem_testing_only_new_decapsulation_key1024(Alloc *a, Slice b, Error *err);

/* EncapsulateInternal: Encapsulate with the 32 byte message m in place of
 * randomness, for crypto/mlkem/mlkemtest. */
BURROW_OWNS(ret) Slice burrow__mlkem_encapsulate_internal768(
    const MlkemEncapsulationKey768 *ek, Alloc *a, const Byte m[32], Slice *ciphertext);
BURROW_OWNS(ret) Slice burrow__mlkem_encapsulate_internal1024(
    const MlkemEncapsulationKey1024 *ek, Alloc *a, const Byte m[32], Slice *ciphertext);

#endif /* BURROW_SRC_CRYPTO_MLKEM_INTERNAL_H */
