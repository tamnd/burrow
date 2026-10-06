/* crypto/cipher, the modes that turn a block cipher into something that can
 * encrypt a message.
 *
 *     Error err;
 *     CipherBlock block = aes_new_cipher(a, key, &err);
 *     CipherStream ctr = cipher_new_ctr(a, block, iv);
 *     cipher_stream_xor_key_stream(ctr, ciphertext, plaintext);
 *
 * A block cipher on its own encrypts exactly one block, 16 bytes for AES, and
 * the same block always comes out the same. The modes here chain blocks
 * together. CTR, OFB and CFB make a key stream that is XORed with the message,
 * so they take any length, and CBC encrypts whole blocks, each one mixed with
 * the one before.
 *
 * None of these modes says whether a message was changed on the way. Someone
 * who can flip bits in a CTR ciphertext flips the same bits in the plaintext,
 * and padding errors in CBC have leaked whole messages. Unless a protocol you
 * are implementing says otherwise, use an AEAD, which is GCM in this package.
 *
 * The four interfaces are Go's, each a vtable and a data pointer as burrow/iface.h
 * describes. Every method takes dst and src the way Go's do: dst may be src
 * exactly or apart from it, and an overlap that is anything else panics with
 * "crypto/cipher: invalid buffer overlap", as in Go.
 *
 * The constructors allocate from a and give back a nil interface when a is out
 * of memory. Nothing they make needs closing.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package crypto/cipher */

#ifndef BURROW_CRYPTO_CIPHER_H
#define BURROW_CRYPTO_CIPHER_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------- interfaces */

/* cipher.Block: a block cipher with its key. Encrypt and Decrypt work on the
 * first block of src and write the first block of dst, and panic when either
 * is shorter than a block. */
typedef struct CipherBlockVT {
    const Type *self_type;
    Int (*block_size)(void *self);
    void (*encrypt)(void *self, Slice dst, Slice src);
    void (*decrypt)(void *self, Slice dst, Slice src);
} CipherBlockVT;

typedef struct CipherBlock {
    const CipherBlockVT *vt;
    void *data;
} CipherBlock;

/* cipher.Stream: a stream cipher. XORKeyStream XORs each byte of src with the
 * next byte of the key stream and writes it to dst, which has to be at least as
 * long as src; only the first src.len bytes of dst are touched. Calls carry on
 * where the last one stopped, so two calls are the same as one on the two
 * inputs joined. */
typedef struct CipherStreamVT {
    const Type *self_type;
    void (*xor_key_stream)(void *self, Slice dst, Slice src);
} CipherStreamVT;

typedef struct CipherStream {
    const CipherStreamVT *vt;
    void *data;
} CipherStream;

/* cipher.BlockMode: a block cipher in a mode that works on whole blocks, which
 * is CBC here. CryptBlocks takes a src whose length is a multiple of the block
 * size and a dst at least as long. Like a Stream, it carries on from the last
 * call. */
typedef struct CipherBlockModeVT {
    const Type *self_type;
    Int (*block_size)(void *self);
    void (*crypt_blocks)(void *self, Slice dst, Slice src);
} CipherBlockModeVT;

typedef struct CipherBlockMode {
    const CipherBlockModeVT *vt;
    void *data;
} CipherBlockMode;

/* cipher.AEAD: authenticated encryption with associated data.
 *
 * Seal encrypts and authenticates plaintext, authenticates additional_data as
 * well, and appends the result to dst, growing it from a when it has to, the
 * way Go's append does. The nonce has to be nonce_size bytes and must never be
 * used twice with the same key. To encrypt in place, pass plaintext with its
 * length cut to 0 as dst.
 *
 * Open checks and decrypts what Seal made, appending the plaintext to dst. When
 * the ciphertext, the nonce or the additional data is not what was sealed, it
 * fails with "cipher: message authentication failed" and returns a nil slice,
 * and dst up to its capacity may have been written over. */
typedef struct CipherAEADVT {
    const Type *self_type;
    Int (*nonce_size)(void *self);
    Int (*overhead)(void *self);
    Slice (*seal)(void *self, Alloc *a, Slice dst, Slice nonce, Slice plaintext,
                  Slice additional_data);
    Slice (*open)(void *self, Alloc *a, Slice dst, Slice nonce, Slice ciphertext,
                  Slice additional_data, Error *err);
} CipherAEADVT;

typedef struct CipherAEAD {
    const CipherAEADVT *vt;
    void *data;
} CipherAEAD;

/* ----------------------------------------------------------- method calls */

static inline Int cipher_block_block_size(CipherBlock b) {
    return b.vt->block_size(b.data);
}

static inline void cipher_block_encrypt(CipherBlock b, Slice dst, Slice src) {
    b.vt->encrypt(b.data, dst, src);
}

static inline void cipher_block_decrypt(CipherBlock b, Slice dst, Slice src) {
    b.vt->decrypt(b.data, dst, src);
}

static inline void cipher_stream_xor_key_stream(CipherStream s, Slice dst, Slice src) {
    s.vt->xor_key_stream(s.data, dst, src);
}

static inline Int cipher_block_mode_block_size(CipherBlockMode m) {
    return m.vt->block_size(m.data);
}

static inline void cipher_block_mode_crypt_blocks(CipherBlockMode m, Slice dst,
                                                  Slice src) {
    m.vt->crypt_blocks(m.data, dst, src);
}

static inline Int cipher_aead_nonce_size(CipherAEAD x) {
    return x.vt->nonce_size(x.data);
}

static inline Int cipher_aead_overhead(CipherAEAD x) {
    return x.vt->overhead(x.data);
}

BURROW_OWNS(ret) BURROW_BORROWS(ret, dst) static inline Slice
cipher_aead_seal(CipherAEAD x, Alloc *a, Slice dst, Slice nonce, Slice plaintext,
                 Slice additional_data) {
    return x.vt->seal(x.data, a, dst, nonce, plaintext, additional_data);
}

BURROW_OWNS(ret) BURROW_BORROWS(ret, dst) static inline Slice
cipher_aead_open(CipherAEAD x, Alloc *a, Slice dst, Slice nonce, Slice ciphertext,
                 Slice additional_data, Error *err) {
    return x.vt->open(x.data, a, dst, nonce, ciphertext, additional_data, err);
}

/* ------------------------------------------------------------------ modes */

/* NewCBCEncrypter and NewCBCDecrypter: cipher block chaining over b, starting
 * from iv, which has to be one block long and is copied. The two panic when it
 * is not, with "cipher.NewCBCEncrypter: IV length must equal block size" and
 * the Decrypter's version of the same. */
BURROW_OWNS(ret) CipherBlockMode cipher_new_cbc_encrypter(Alloc *a, CipherBlock b,
                                                          Slice iv);
BURROW_OWNS(ret) CipherBlockMode cipher_new_cbc_decrypter(Alloc *a, CipherBlock b,
                                                          Slice iv);

/* The SetIV method Go's CBC modes have outside the BlockMode interface, which
 * starts m again from iv without making a new one. m has to be from one of the
 * two functions above, and iv one block long, or it panics. */
void cipher_cbc_set_iv(CipherBlockMode m, Slice iv);

/* NewCTR: counter mode over b, with iv, one block long, as the first counter.
 * The counter is the whole block taken as a big endian number, and it wraps. */
BURROW_OWNS(ret) CipherStream cipher_new_ctr(Alloc *a, CipherBlock b, Slice iv);

/* NewOFB: output feedback mode over b, starting from iv, one block long.
 *
 * Deprecated, as in Go: OFB is not authenticated and is no faster than CTR. */
BURROW_OWNS(ret) CipherStream cipher_new_ofb(Alloc *a, CipherBlock b, Slice iv);

/* NewCFBEncrypter and NewCFBDecrypter: cipher feedback mode over b, starting
 * from iv, one block long.
 *
 * Deprecated, as in Go: CFB is not authenticated. Use CTR if a stream is what
 * you need. */
BURROW_OWNS(ret) CipherStream cipher_new_cfb_encrypter(Alloc *a, CipherBlock b,
                                                       Slice iv);
BURROW_OWNS(ret) CipherStream cipher_new_cfb_decrypter(Alloc *a, CipherBlock b,
                                                       Slice iv);

/* -------------------------------------------------------------------- GCM */

/* NewGCM: b, which has to have a 16 byte block, in Galois/Counter Mode with the
 * standard 12 byte nonce and 16 byte tag. When b is from aes_new_cipher the
 * work goes to the AES and carryless multiply instructions where there are
 * some. Any other block works through its methods, more slowly.
 *
 * A block of any other size gives a nil AEAD and the error "cipher: NewGCM
 * requires 128-bit block cipher", in the calling goroutine's error arena.
 *
 * Never seal more than 2^32 messages with one key and random nonces: past that
 * the chance of a repeated nonce, which gives the key away, is too big. Where a
 * counter is not at hand, cipher_new_gcm_with_random_nonce picks the nonces. */
BURROW_OWNS(ret) CipherAEAD cipher_new_gcm(Alloc *a, CipherBlock b, Error *err);

/* NewGCMWithNonceSize: GCM with nonces size bytes long. Only use this to talk
 * to something that already uses a nonce of that size. A size of 0 or less is
 * the error "cipher: the nonce can't have zero length". */
BURROW_OWNS(ret) CipherAEAD cipher_new_gcm_with_nonce_size(Alloc *a, CipherBlock b,
                                                           Int size, Error *err);

/* NewGCMWithTagSize: GCM with tags tag_size bytes long, from 12 to 16. Anything
 * else is the error "cipher: incorrect tag size given to GCM". Only use this to
 * talk to something that already uses a tag of that size. */
BURROW_OWNS(ret) CipherAEAD cipher_new_gcm_with_tag_size(Alloc *a, CipherBlock b,
                                                         Int tag_size, Error *err);

/* NewGCMWithRandomNonce: GCM that picks a random 12 byte nonce for every Seal
 * and puts it in front of the ciphertext, where Open finds it. Its nonce size
 * is 0, so pass an empty nonce to both, and its overhead is 28 bytes, the nonce
 * and the tag. b has to come from aes_new_cipher, or this is the error
 * "cipher: NewGCMWithRandomNonce requires aes.Block".
 *
 * Seal no more than 2^32 messages with one key, as with cipher_new_gcm. */
BURROW_OWNS(ret) CipherAEAD cipher_new_gcm_with_random_nonce(Alloc *a, CipherBlock b,
                                                             Error *err);

/* ---------------------------------------------------------------- streams */

/* cipher.StreamReader: r with every byte read from it passed through s. Fill in
 * the two fields, as in Go, and read with cipher_stream_reader_read or through
 * cipher_stream_reader_as_io_reader. */
typedef struct CipherStreamReader {
    CipherStream s;
    IoReader r;
} CipherStreamReader;

/* StreamReader.Read: reads into dst from r and XORs what came back with s. */
Int cipher_stream_reader_read(CipherStreamReader r, Slice dst, Error *err);

/* r as an io.Reader. r has to outlive what this returns. */
BURROW_BORROWS(ret, r) IoReader
cipher_stream_reader_as_io_reader(CipherStreamReader *r);

/* cipher.StreamWriter: every byte written goes through s and then to w. err is
 * Go's Err field, which nothing reads.
 *
 * Go's Close asks whether W is also an io.Closer. An interface in C cannot be
 * asked for a different one, so closer is where the Closer goes: set it to w's
 * Close, or leave it nil when w has none. */
typedef struct CipherStreamWriter {
    CipherStream s;
    IoWriter w;
    Error err;
    IoCloser closer;
} CipherStreamWriter;

/* StreamWriter.Write: src through s and then to w in one write. A short write
 * means the stream and w no longer agree and the writer has to be thrown away,
 * as in Go. */
Int cipher_stream_writer_write(CipherStreamWriter w, Slice src, Error *err);

/* StreamWriter.Close: closer's Close, or no error when closer is nil. The error
 * is whatever closer's Close gave back. */
BURROW_BORROWS(ret) Error cipher_stream_writer_close(CipherStreamWriter w);

/* w as an io.Writer. w has to outlive what this returns. */
BURROW_BORROWS(ret, w) IoWriter
cipher_stream_writer_as_io_writer(CipherStreamWriter *w);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_CRYPTO_CIPHER_H */
