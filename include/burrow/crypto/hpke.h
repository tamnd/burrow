/* crypto/hpke, Hybrid Public Key Encryption as RFC 9180 has it, with the
 * post-quantum KEMs of draft-ietf-hpke-pq.
 *
 *     // The recipient makes a key and publishes the public half.
 *     const HpkeKEM *kem = hpke_mlkem768_x25519();
 *     HpkePrivateKey *k = hpke_kem_generate_key(kem, a, &err);
 *     Slice pub = hpke_public_key_bytes(hpke_private_key_public_key(k), a);
 *
 *     // The sender seals a message to it.
 *     HpkePublicKey *pk = hpke_kem_new_public_key(kem, a, pub, &err);
 *     Slice ct = hpke_seal(a, pk, hpke_hkdfsha256(), hpke_aes256_gcm(), info,
 *                          message, &err);
 *
 *     // The recipient opens it.
 *     Slice pt = hpke_open(a, k, hpke_hkdfsha256(), hpke_aes256_gcm(), info, ct,
 *                          &err);
 *
 * A ciphersuite is a KEM, a KDF and an AEAD. Each is one of the values the
 * functions below return and nothing else, as Go's are interfaces with
 * unexported methods that no other package can implement. Comparing two with
 * == tells whether they are the same.
 *
 * Only the base mode is here, which is all Go has: no PSK and no
 * authenticated modes.
 *
 * Keys, senders and recipients are allocated from the Alloc they are made
 * with. A key made from bytes, generated or derived owns the crypto/ecdh and
 * crypto/mlkem keys inside it, and hpke_public_key_free and
 * hpke_private_key_free free those along with it. A key made with one of the
 * hpke_new_dhkem, hpke_new_hybrid and hpke_new_mlkem functions borrows the
 * keys it is given, which have to outlive it, and freeing it leaves them
 * alone. A Sender or Recipient holds a cipher.AEAD, which like the rest of
 * crypto/cipher has nothing to close, so they are meant for an arena too.
 *
 * Copyright 2024 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package crypto/hpke */

#ifndef BURROW_CRYPTO_HPKE_H
#define BURROW_CRYPTO_HPKE_H

#include "burrow/core.h"
#include "burrow/crypto.h"
#include "burrow/crypto/ecdh.h"
#include "burrow/error.h"
#include "burrow/mem.h"
#include "burrow/slice.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------- types */

/* hpke.KEM: a Key Encapsulation Mechanism, the part of a ciphersuite that
 * agrees on a shared secret. */
typedef struct HpkeKEM HpkeKEM;

/* hpke.KDF: the key derivation function of a ciphersuite. */
typedef struct HpkeKDF HpkeKDF;

/* hpke.AEAD: the symmetric encryption of a ciphersuite. */
typedef struct HpkeAEAD HpkeAEAD;

/* hpke.PublicKey: a KEM with an encapsulation key, the public key. */
typedef struct HpkePublicKey HpkePublicKey;

/* hpke.PrivateKey: a KEM with a decapsulation key, the secret key. */
typedef struct HpkePrivateKey HpkePrivateKey;

/* hpke.Sender: the sending side of an HPKE context, for one public key. It
 * counts the messages it seals, so each gets its own nonce. */
typedef struct HpkeSender HpkeSender;

/* hpke.Recipient: the receiving side of an HPKE context, for one private key.
 * It counts the messages it opens, and has to open them in the order they were
 * sealed. */
typedef struct HpkeRecipient HpkeRecipient;

/* -------------------------------------------------------------------- KEMs */

/* NewKEM: the KEM with the given HPKE identifier, which is one of 0x0010,
 * 0x0011, 0x0012 and 0x0020 for DHKEM over P-256, P-384, P-521 and X25519,
 * 0x0041 and 0x0042 for ML-KEM-768 and ML-KEM-1024, and 0x647a, 0x0050 and
 * 0x0051 for the hybrids. Anything else is NULL and the error "unsupported
 * KEM". */
BURROW_STATIC(ret) const HpkeKEM *hpke_new_kem(uint16_t id, Error *err);

/* DHKEM: DHKEM(P-256, HKDF-SHA256), DHKEM(P-384, HKDF-SHA384), DHKEM(P-521,
 * HKDF-SHA512) or DHKEM(X25519, HKDF-SHA256), depending on curve. A curve that
 * is none of those gives a KEM with the identifier 0 that does nothing but
 * fail with "unsupported curve", as Go's does. */
BURROW_STATIC(ret) const HpkeKEM *hpke_dhkem(const EcdhCurve *curve);

/* MLKEM768 and MLKEM1024: ML-KEM on its own, from draft-ietf-hpke-pq. */
BURROW_STATIC(ret) const HpkeKEM *hpke_mlkem768(void);
BURROW_STATIC(ret) const HpkeKEM *hpke_mlkem1024(void);

/* MLKEM768X25519, also known as X-Wing, MLKEM768P256 and MLKEM1024P384: the
 * hybrids of ML-KEM with an elliptic curve, from draft-ietf-hpke-pq. */
BURROW_STATIC(ret) const HpkeKEM *hpke_mlkem768_x25519(void);
BURROW_STATIC(ret) const HpkeKEM *hpke_mlkem768_p256(void);
BURROW_STATIC(ret) const HpkeKEM *hpke_mlkem1024_p384(void);

/* KEM.ID: the HPKE identifier of kem. */
uint16_t hpke_kem_id(const HpkeKEM *kem);

/* KEM.GenerateKey: a new key pair, from the system's random generator. */
BURROW_OWNS(ret) HpkePrivateKey *hpke_kem_generate_key(const HpkeKEM *kem, Alloc *a,
                                                       Error *err);

/* KEM.NewPublicKey: the public key with the encoding data, which is
 * DeserializePublicKey of RFC 9180. */
BURROW_OWNS(ret) HpkePublicKey *hpke_kem_new_public_key(const HpkeKEM *kem, Alloc *a,
                                                        Slice data, Error *err);

/* KEM.NewPrivateKey: the private key with the encoding data, which is
 * DeserializePrivateKey of RFC 9180. For ML-KEM it is the 64 byte seed, and
 * for the hybrids a 32 byte secret that both keys are derived from. */
BURROW_OWNS(ret) HpkePrivateKey *hpke_kem_new_private_key(const HpkeKEM *kem, Alloc *a,
                                                          Slice data, Error *err);

/* KEM.DeriveKeyPair: the key pair that the input keying material ikm gives,
 * always the same one for the same ikm, as RFC 9180 defines it. */
BURROW_OWNS(ret) HpkePrivateKey *hpke_kem_derive_key_pair(const HpkeKEM *kem, Alloc *a,
                                                          Slice ikm, Error *err);

/* -------------------------------------------------------------------- KDFs */

/* NewKDF: the KDF with the given HPKE identifier, 0x0001 to 0x0003 for HKDF
 * with SHA-256, SHA-384 and SHA-512 and 0x0010 and 0x0011 for SHAKE128 and
 * SHAKE256. Anything else is NULL and the error "unsupported KDF 00ff", with
 * the identifier in hex. */
BURROW_STATIC(ret) const HpkeKDF *hpke_new_kdf(uint16_t id, Error *err);

/* HKDFSHA256, HKDFSHA384 and HKDFSHA512: HKDF with each hash. */
BURROW_STATIC(ret) const HpkeKDF *hpke_hkdfsha256(void);
BURROW_STATIC(ret) const HpkeKDF *hpke_hkdfsha384(void);
BURROW_STATIC(ret) const HpkeKDF *hpke_hkdfsha512(void);

/* SHAKE128 and SHAKE256: the one-stage KDFs of draft-ietf-hpke-pq. */
BURROW_STATIC(ret) const HpkeKDF *hpke_shake128(void);
BURROW_STATIC(ret) const HpkeKDF *hpke_shake256(void);

/* KDF.ID: the HPKE identifier of kdf. */
uint16_t hpke_kdf_id(const HpkeKDF *kdf);

/* ------------------------------------------------------------------- AEADs */

/* NewAEAD: the AEAD with the given HPKE identifier, 0x0001 for AES-128-GCM,
 * 0x0002 for AES-256-GCM, 0x0003 for ChaCha20-Poly1305 and 0xffff for export
 * only. Anything else is NULL and the error "unsupported AEAD 00ff", with the
 * identifier in hex. */
BURROW_STATIC(ret) const HpkeAEAD *hpke_new_aead(uint16_t id, Error *err);

/* AES128GCM, AES256GCM and ChaCha20Poly1305: the three AEADs. */
BURROW_STATIC(ret) const HpkeAEAD *hpke_aes128_gcm(void);
BURROW_STATIC(ret) const HpkeAEAD *hpke_aes256_gcm(void);
BURROW_STATIC(ret) const HpkeAEAD *hpke_cha_cha20_poly1305(void);

/* ExportOnly: an AEAD that cannot encrypt or decrypt anything, for a context
 * that is only there to export secrets. Seal and Open with it fail with
 * "export-only instantiation". */
BURROW_STATIC(ret) const HpkeAEAD *hpke_export_only(void);

/* AEAD.ID: the HPKE identifier of aead. */
uint16_t hpke_aead_id(const HpkeAEAD *aead);

/* -------------------------------------------------------------------- keys */

/* NewDHKEMPublicKey: pub as the public key of the DHKEM for its curve. pub is
 * borrowed and has to outlive the key. */
BURROW_OWNS(ret) HpkePublicKey *
hpke_new_dhkem_public_key(Alloc *a, const EcdhPublicKey *pub, Error *err);

/* NewDHKEMPrivateKey: priv as the private key of the DHKEM for its curve. priv
 * can be any EcdhKeyExchanger, a key in hardware for one, and is borrowed. Its
 * Bytes only works when priv is a crypto/ecdh private key. */
BURROW_OWNS(ret) HpkePrivateKey *
hpke_new_dhkem_private_key(Alloc *a, EcdhKeyExchanger priv, Error *err);

/* NewHybridPublicKey: the public key of MLKEM768-X25519, MLKEM768-P256 or
 * MLKEM1024-P384, depending on the curve of t, which pq has to match: an
 * ML-KEM-768 encapsulation key for the first two and an ML-KEM-1024 one for
 * the third, or it is an error like "invalid PQ KEM for X25519 hybrid". Both
 * are borrowed. */
BURROW_OWNS(ret) HpkePublicKey *hpke_new_hybrid_public_key(Alloc *a,
                                                           CryptoEncapsulator pq,
                                                           const EcdhPublicKey *t,
                                                           Error *err);

/* NewHybridPrivateKey: the private key of one of the hybrids, as
 * hpke_new_hybrid_public_key picks it, from any decapsulator and key
 * exchanger, which are borrowed. Such a key has no seed, so its Bytes fails. */
BURROW_OWNS(ret) HpkePrivateKey *hpke_new_hybrid_private_key(Alloc *a,
                                                             CryptoDecapsulator pq,
                                                             EcdhKeyExchanger t,
                                                             Error *err);

/* NewMLKEMPublicKey: pub, an ML-KEM-768 or ML-KEM-1024 encapsulation key, as
 * the public key of that KEM. Any other encapsulator is the error "unsupported
 * public key type". pub is borrowed. */
BURROW_OWNS(ret) HpkePublicKey *
hpke_new_mlkem_public_key(Alloc *a, CryptoEncapsulator pub, Error *err);

/* NewMLKEMPrivateKey: priv as the private key of ML-KEM-768 or ML-KEM-1024,
 * from the type of its encapsulator. priv is borrowed. */
BURROW_OWNS(ret) HpkePrivateKey *
hpke_new_mlkem_private_key(Alloc *a, CryptoDecapsulator priv, Error *err);

/* Frees k and whatever keys inside it it owns. NULL is fine. */
void hpke_public_key_free(HpkePublicKey *k);
void hpke_private_key_free(HpkePrivateKey *k);

/* PublicKey.KEM: the KEM of pk. */
BURROW_STATIC(ret) const HpkeKEM *hpke_public_key_kem(const HpkePublicKey *pk);

/* PublicKey.Bytes: the encoding of pk, which is SerializePublicKey of RFC
 * 9180. */
BURROW_OWNS(ret) Slice hpke_public_key_bytes(const HpkePublicKey *pk, Alloc *a);

/* PrivateKey.KEM: the KEM of k. */
BURROW_STATIC(ret) const HpkeKEM *hpke_private_key_kem(const HpkePrivateKey *k);

/* PrivateKey.Bytes: the encoding of k, which is SerializePrivateKey of RFC
 * 9180. For X25519 that is clamped as the RFC says it has to be, so it may not
 * be what the key was made from. A key with no encoding to give, like one
 * made from a key exchanger that is not a crypto/ecdh key, is an error. */
BURROW_OWNS(ret) Slice hpke_private_key_bytes(const HpkePrivateKey *k, Alloc *a,
                                              Error *err);

/* PrivateKey.PublicKey: the public key of k, which is part of k. */
BURROW_BORROWS(ret, k) const HpkePublicKey *
hpke_private_key_public_key(const HpkePrivateKey *k);

/* ---------------------------------------------------------------- contexts */

/* NewSender: a sending context for pk with the ciphersuite of its KEM, kdf
 * and aead, and the encapsulated key enc that the recipient needs to make the
 * matching context. info is public data that both sides have to agree on. */
BURROW_OWNS(ret) Slice hpke_new_sender(Alloc *a, const HpkePublicKey *pk,
                                       const HpkeKDF *kdf, const HpkeAEAD *aead,
                                       Slice info, HpkeSender **s, Error *err);

/* NewRecipient: the receiving context for k that matches the sender that made
 * enc, with the same kdf, aead and info. */
BURROW_OWNS(ret) HpkeRecipient *
hpke_new_recipient(Alloc *a, Slice enc, const HpkePrivateKey *k, const HpkeKDF *kdf,
                   const HpkeAEAD *aead, Slice info, Error *err);

/* Sender.Seal: plaintext encrypted, with aad authenticated along with it. Each
 * call takes the next nonce, and the recipient has to open the messages in the
 * same order. */
BURROW_OWNS(ret) Slice hpke_sender_seal(HpkeSender *s, Alloc *a, Slice aad,
                                        Slice plaintext, Error *err);

/* Sender.Export: length bytes of secret derived from what the two sides share
 * and exporter_context. length is from 0 to 65535, or it is the error "invalid
 * length", and the KDF may allow less than that. */
BURROW_OWNS(ret) Slice hpke_sender_export(const HpkeSender *s, Alloc *a,
                                          Str exporter_context, Int length, Error *err);

/* Recipient.Open: the plaintext of ciphertext, which has to have been sealed
 * with aad. Only a message that opens moves the recipient on to the next
 * nonce. */
BURROW_OWNS(ret) Slice hpke_recipient_open(HpkeRecipient *r, Alloc *a, Slice aad,
                                           Slice ciphertext, Error *err);

/* Recipient.Export: the same secret the sender's Export gives. */
BURROW_OWNS(ret) Slice hpke_recipient_export(const HpkeRecipient *r, Alloc *a,
                                             Str exporter_context, Int length,
                                             Error *err);

/* Seal: a one-off hpke_new_sender and hpke_sender_seal with no aad, returning
 * the encapsulated key and the ciphertext one after the other. */
BURROW_OWNS(ret) Slice hpke_seal(Alloc *a, const HpkePublicKey *pk, const HpkeKDF *kdf,
                                 const HpkeAEAD *aead, Slice info, Slice plaintext,
                                 Error *err);

/* Open: the other side of hpke_seal, for ciphertext that starts with the
 * encapsulated key. One shorter than that is "ciphertext too short". */
BURROW_OWNS(ret) Slice hpke_open(Alloc *a, const HpkePrivateKey *k, const HpkeKDF *kdf,
                                 const HpkeAEAD *aead, Slice info, Slice ciphertext,
                                 Error *err);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_CRYPTO_HPKE_H */
