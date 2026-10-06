/* crypto, the constants and interfaces the crypto packages share.
 *
 *     Hash h = crypto_hash_new(CRYPTO_SHA256, a);
 *     Int n = crypto_hash_size(CRYPTO_SHA512); // 64
 *
 * CryptoHash names a hash function by number, so that a signature scheme or a
 * certificate can say which one it used without depending on the package that
 * implements it. CryptoSigner and CryptoDecrypter are what a private key kept
 * somewhere else, a hardware module or another process, looks like to the
 * code that uses it.
 *
 * Go links a hash in only when something imports its package, and the package
 * registers it from init. burrow is one library, so every hash it implements
 * is always there: MD5, SHA-1, the SHA-2 family and SHA-3. MD4, RIPEMD-160 and
 * the BLAKE2 hashes are not in Go's standard library either, and are only
 * available once a program registers them with crypto_register_hash.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package crypto */

#ifndef BURROW_CRYPTO_H
#define BURROW_CRYPTO_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/hash.h"
#include "burrow/iface.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#ifdef __cplusplus
extern "C" {
#endif

/* crypto.Hash: a hash function implemented somewhere else, by number. */
typedef Uint CryptoHash;

extern const Type burrow_type_CryptoHash;
#define TYPE_CRYPTO_HASH TYPE_OF(CryptoHash)

#define CRYPTO_MD4 ((CryptoHash)1)         /* not in burrow, register your own */
#define CRYPTO_MD5 ((CryptoHash)2)         /* burrow/crypto/md5.h */
#define CRYPTO_SHA1 ((CryptoHash)3)        /* burrow/crypto/sha1.h */
#define CRYPTO_SHA224 ((CryptoHash)4)      /* burrow/crypto/sha256.h */
#define CRYPTO_SHA256 ((CryptoHash)5)      /* burrow/crypto/sha256.h */
#define CRYPTO_SHA384 ((CryptoHash)6)      /* burrow/crypto/sha512.h */
#define CRYPTO_SHA512 ((CryptoHash)7)      /* burrow/crypto/sha512.h */
#define CRYPTO_MD5SHA1 ((CryptoHash)8)     /* no implementation, MD5+SHA1 for TLS RSA */
#define CRYPTO_RIPEMD160 ((CryptoHash)9)   /* not in burrow, register your own */
#define CRYPTO_SHA3_224 ((CryptoHash)10)   /* burrow/crypto/sha3.h */
#define CRYPTO_SHA3_256 ((CryptoHash)11)   /* burrow/crypto/sha3.h */
#define CRYPTO_SHA3_384 ((CryptoHash)12)   /* burrow/crypto/sha3.h */
#define CRYPTO_SHA3_512 ((CryptoHash)13)   /* burrow/crypto/sha3.h */
#define CRYPTO_SHA512_224 ((CryptoHash)14) /* burrow/crypto/sha512.h */
#define CRYPTO_SHA512_256 ((CryptoHash)15) /* burrow/crypto/sha512.h */
#define CRYPTO_BLAKE2_S256 ((CryptoHash)16) /* not in burrow, register your own */
#define CRYPTO_BLAKE2_B256 ((CryptoHash)17) /* not in burrow, register your own */
#define CRYPTO_BLAKE2_B384 ((CryptoHash)18) /* not in burrow, register your own */
#define CRYPTO_BLAKE2_B512 ((CryptoHash)19) /* not in burrow, register your own */

/* crypto.MLDSAMu: not a hash but a marker, for a message that is already ML-DSA's
 * μ representative (RFC 9881). It has a size and a name and can never be
 * registered or made. */
#define CRYPTO_MLDSA_MU ((CryptoHash)20)

/* crypto.Hash.HashFunc: h itself, which is what makes a CryptoHash a
 * CryptoSignerOpts. */
CryptoHash crypto_hash_hash_func(CryptoHash h);

/* crypto.Hash.String: "SHA-256" and so on, or "unknown hash value 42" built in
 * a for a number that is not one of the above. */
BURROW_OWNS(ret) Str crypto_hash_string(CryptoHash h, Alloc *a);

/* crypto.Hash.Size: the length of h's digest in bytes, whether or not h is
 * available. Panics with "crypto: Size of unknown hash function" for a number
 * that is not one of the above. */
Int crypto_hash_size(CryptoHash h);

/* crypto.Hash.New: a new Hash computing h, allocated from a. Panics with
 * "crypto: requested hash function unavailable: " and h's name when h is not
 * available. A nil Hash when a refuses. */
BURROW_OWNS(ret) Hash crypto_hash_new(CryptoHash h, Alloc *a);

/* crypto.Hash.Available: whether crypto_hash_new will make h. True for the
 * hashes burrow has and for any registered with crypto_register_hash. */
bool crypto_hash_available(CryptoHash h);

/* crypto.RegisterHash: makes f what crypto_hash_new calls for h, in place of
 * burrow's own if it has one. It is safe to call while other threads are
 * making hashes. A NULL f makes h unavailable, burrow's own included. Panics
 * for 0, a number past the last hash, or CRYPTO_MLDSA_MU. */
void crypto_register_hash(CryptoHash h, HashNewFunc f);

/* crypto.PublicKey and crypto.PrivateKey: a key of any algorithm. Every key
 * type in the library also has an equal function, which Go puts in the method
 * set as Equal. */
typedef Any CryptoPublicKey;
typedef Any CryptoPrivateKey;

/* crypto.SignerOpts: options for signing, of which the hash the message went
 * through is the one every signer reads. Zero means it was not hashed. */
typedef struct CryptoSignerOptsVT {
    const Type *self_type;
    CryptoHash (*hash_func)(void *self);
} CryptoSignerOptsVT;

typedef struct CryptoSignerOpts {
    const CryptoSignerOptsVT *vt;
    void *data;
} CryptoSignerOpts;

/* *h as a CryptoSignerOpts, the usual way to say which hash a digest came
 * from. It points at h, so h has to outlive it. */
BURROW_BORROWS(ret, h) CryptoSignerOpts crypto_hash_as_signer_opts(const CryptoHash *h);

/* crypto.Signer: a private key that signs, possibly one that never leaves a
 * hardware module.
 *
 * public_key gives the matching public key. sign signs digest, the output of the
 * hash opts names, with entropy from rand if the scheme wants it, and returns
 * the signature allocated from a: PKCS #1 v1.5 or PSS for RSA, as opts says,
 * and DER encoded ASN.1 for ECDSA. A message longer than one digest has to be
 * hashed by the caller first, or signed with crypto_sign_message. */
typedef struct CryptoSignerVT {
    const Type *self_type;
    CryptoPublicKey (*public_key)(void *self);
    Slice (*sign)(void *self, Alloc *a, IoReader rand, Slice digest,
                  CryptoSignerOpts opts, Error *err);
} CryptoSignerVT;

typedef struct CryptoSigner {
    const CryptoSignerVT *vt;
    void *data;
} CryptoSigner;

static inline CryptoPublicKey crypto_signer_public(CryptoSigner s) {
    return s.vt->public_key(s.data);
}

BURROW_OWNS(ret) static inline Slice crypto_signer_sign(CryptoSigner s, Alloc *a,
                                                        IoReader rand, Slice digest,
                                                        CryptoSignerOpts opts,
                                                        Error *err) {
    return s.vt->sign(s.data, a, rand, digest, opts, err);
}

/* crypto.MessageSigner: a Signer that takes the whole message and hashes it
 * itself. sign_message and sign give the same signature for the same opts,
 * and a key that cannot sign a digest has sign return an error. */
typedef struct CryptoMessageSignerVT {
    CryptoSignerVT signer;
    Slice (*sign_message)(void *self, Alloc *a, IoReader rand, Slice msg,
                          CryptoSignerOpts opts, Error *err);
} CryptoMessageSignerVT;

typedef struct CryptoMessageSigner {
    const CryptoMessageSignerVT *vt;
    void *data;
} CryptoMessageSigner;

BURROW_OWNS(ret) static inline Slice
crypto_message_signer_sign_message(CryptoMessageSigner s, Alloc *a, IoReader rand,
                                   Slice msg, CryptoSignerOpts opts, Error *err) {
    return s.vt->sign_message(s.data, a, rand, msg, opts, err);
}

static inline CryptoPublicKey crypto_message_signer_public(CryptoMessageSigner s) {
    return s.vt->signer.public_key(s.data);
}

BURROW_OWNS(ret) static inline Slice
crypto_message_signer_sign(CryptoMessageSigner s, Alloc *a, IoReader rand, Slice digest,
                           CryptoSignerOpts opts, Error *err) {
    return s.vt->signer.sign(s.data, a, rand, digest, opts, err);
}

static inline CryptoSigner crypto_message_signer_as_signer(CryptoMessageSigner s) {
    CryptoSigner out = {&s.vt->signer, s.data};
    return out;
}

/* crypto.SignMessage: signs msg with signer. When the signer's type has a
 * SignMessage method in its method set, with the shape below, that is called
 * with msg as it is. Otherwise msg goes through the hash opts names, unless
 * that is zero, and the digest goes to sign. A hash that is not available is
 * an error, "crypto: requested hash function unavailable: " and its name.
 *
 * A C vtable cannot be asked what else its type implements, which is what Go
 * does here with signer.(MessageSigner). The method set on the type's
 * descriptor can, the same way io_copy finds WriterTo:
 *
 *     static Slice my_sign_message(MyKey *k, CryptoAllocArg a, IoReader rand,
 *                                  Bytes msg, CryptoSignerOpts opts,
 *                                  CryptoErrorArg err);
 *
 *     #define MY_KEY_METHODS(M, T)                                             \
 *         M(T, SignMessage, my_sign_message, CRYPTO_SIG_SIGN_MESSAGE)
 *     BURROW_STRUCT_DEFINE_METHODS(MyKey, MY_KEY_FIELDS, MY_KEY_METHODS);
 *
 * and the signer's vtable names TYPE_OF(MyKey) as its self_type. */
BURROW_OWNS(ret) Slice crypto_sign_message(Alloc *a, CryptoSigner signer, IoReader rand,
                                           Slice msg, CryptoSignerOpts opts,
                                           Error *err);

/* Alloc * and Error * under one word each, since a signature list names its
 * types by a single token, and the descriptors that list needs. */
typedef Alloc *CryptoAllocArg;
typedef Error *CryptoErrorArg;
extern const Type burrow_type_CryptoAllocArg;
extern const Type burrow_type_CryptoErrorArg;
extern const Type burrow_type_CryptoSignerOpts;

#define CRYPTO_SIG_SIGN_MESSAGE(IN, OUT)                                               \
    IN(0, CryptoAllocArg)                                                              \
    IN(1, IoReader)                                                                    \
    IN(2, Bytes) IN(3, CryptoSignerOpts) IN(4, CryptoErrorArg) OUT(Bytes)

/* crypto.DecrypterOpts: options for decrypting, which depend on the scheme. */
typedef Any CryptoDecrypterOpts;

/* crypto.Decrypter: a private key that decrypts, possibly one that never
 * leaves a hardware module. decrypt returns the plaintext allocated from a. */
typedef struct CryptoDecrypterVT {
    const Type *self_type;
    CryptoPublicKey (*public_key)(void *self);
    Slice (*decrypt)(void *self, Alloc *a, IoReader rand, Slice msg,
                     CryptoDecrypterOpts opts, Error *err);
} CryptoDecrypterVT;

typedef struct CryptoDecrypter {
    const CryptoDecrypterVT *vt;
    void *data;
} CryptoDecrypter;

static inline CryptoPublicKey crypto_decrypter_public(CryptoDecrypter d) {
    return d.vt->public_key(d.data);
}

BURROW_OWNS(ret) static inline Slice
crypto_decrypter_decrypt(CryptoDecrypter d, Alloc *a, IoReader rand, Slice msg,
                         CryptoDecrypterOpts opts, Error *err) {
    return d.vt->decrypt(d.data, a, rand, msg, opts, err);
}

/* crypto.Encapsulator: the public half of a KEM key, ML-KEM's for one.
 * encapsulate makes a fresh shared key and the ciphertext that carries it,
 * both allocated from a. */
typedef struct CryptoEncapsulateResult {
    Slice shared_key, ciphertext;
} CryptoEncapsulateResult;

typedef struct CryptoEncapsulatorVT {
    const Type *self_type;
    Slice (*bytes)(void *self, Alloc *a);
    CryptoEncapsulateResult (*encapsulate)(void *self, Alloc *a);
} CryptoEncapsulatorVT;

typedef struct CryptoEncapsulator {
    const CryptoEncapsulatorVT *vt;
    void *data;
} CryptoEncapsulator;

BURROW_OWNS(ret) static inline Slice crypto_encapsulator_bytes(CryptoEncapsulator e,
                                                               Alloc *a) {
    return e.vt->bytes(e.data, a);
}

static inline CryptoEncapsulateResult
crypto_encapsulator_encapsulate(CryptoEncapsulator e, Alloc *a) {
    return e.vt->encapsulate(e.data, a);
}

/* crypto.Decapsulator: the private half of a KEM key, possibly one that never
 * leaves a hardware module. decapsulate recovers the shared key from a
 * ciphertext. */
typedef struct CryptoDecapsulatorVT {
    const Type *self_type;
    CryptoEncapsulator (*encapsulator)(void *self);
    Slice (*decapsulate)(void *self, Alloc *a, Slice ciphertext, Error *err);
} CryptoDecapsulatorVT;

typedef struct CryptoDecapsulator {
    const CryptoDecapsulatorVT *vt;
    void *data;
} CryptoDecapsulator;

static inline CryptoEncapsulator
crypto_decapsulator_encapsulator(CryptoDecapsulator d) {
    return d.vt->encapsulator(d.data);
}

BURROW_OWNS(ret) static inline Slice
crypto_decapsulator_decapsulate(CryptoDecapsulator d, Alloc *a, Slice ciphertext,
                                Error *err) {
    return d.vt->decapsulate(d.data, a, ciphertext, err);
}

#ifdef __cplusplus
}
#endif

#endif /* BURROW_CRYPTO_H */
