/* Derived from Go's src/crypto/cipher/cbc.go, cfb.go, ctr.go, ofb.go and io.go,
 * and src/crypto/internal/fips140/aes/cbc.go and ctr.go.
 * Go source: go1.27.1.
 *
 * Each mode has two halves, as in Go: the general one that works through the
 * Block interface a block at a time, and the one crypto/aes gets, which hands
 * several blocks at once to the AES code where the mode allows it. CTR and CBC
 * decryption do; CBC encryption cannot, since every block needs the one before.
 * The panics are Go's, and differ between the halves where Go's do.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/crypto/cipher.h"

#include "burrow/core.h"
#include "burrow/crypto/aes.h"
#include "burrow/crypto/subtle.h"
#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/mem/heap.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include "aes_internal.h"

#include <stdint.h>
#include <string.h>

/* The most bytes of key stream CTR and OFB make at once. */
#define CIPHER_STREAM_BUFFER 512

/* Go's alias.InexactOverlap on two runs of n bytes. */
static bool cipher_inexact_overlap(const void *x, const void *y, Int n) {
    if (n <= 0)
        return false;
    uintptr_t a = (uintptr_t)x;
    uintptr_t b = (uintptr_t)y;
    if (a == b)
        return false;
    return a <= b + (uintptr_t)(n - 1) && b <= a + (uintptr_t)(n - 1);
}

static Slice cipher_bytes(Byte *p, Int n) {
    return slice_from(p, n, n, TYPE_BYTE);
}

/* -------------------------------------------------------------------- CBC */

typedef struct CipherCbc {
    CipherBlock b;
    AesBlock *aes;
    Int block_size;
    Byte *iv;
    Byte *tmp;
} CipherCbc;

static CipherCbc *cipher_cbc_make(Alloc *a, CipherBlock b, Slice iv, Str bad_iv) {
    Int bs = cipher_block_block_size(b);
    if (iv.len != bs)
        panic_str(bad_iv);
    CipherCbc *x = (CipherCbc *)mem_alloc(a, sizeof(CipherCbc) + 2 * (size_t)bs,
                                          _Alignof(CipherCbc));
    if (x == NULL)
        return NULL;
    x->b = b;
    x->aes = burrow__aes_block_of(b);
    x->block_size = bs;
    x->iv = (Byte *)(x + 1);
    x->tmp = x->iv + bs;
    memcpy(x->iv, iv.p, (size_t)bs);
    return x;
}

static void cipher_cbc_check(const CipherCbc *x, Slice dst, Slice src) {
    if (src.len % x->block_size != 0)
        panic_str(BURROW_S("crypto/cipher: input not full blocks"));
    if (dst.len < src.len)
        panic_str(BURROW_S("crypto/cipher: output smaller than input"));
    if (cipher_inexact_overlap(dst.p, src.p, src.len))
        panic_str(BURROW_S("crypto/cipher: invalid buffer overlap"));
}

static Int cipher_cbc_block_size(void *self) {
    return ((CipherCbc *)self)->block_size;
}

static void cipher_cbc_encrypt(void *self, Slice dst, Slice src) {
    CipherCbc *x = (CipherCbc *)self;
    cipher_cbc_check(x, dst, src);
    Int bs = x->block_size;
    Byte *d = (Byte *)dst.p;
    Byte *s = (Byte *)src.p;
    Byte *iv = x->iv;
    for (Int off = 0; off < src.len; off += bs) {
        /* Write the xor to dst, then encrypt in place. */
        subtle_xor_bytes(cipher_bytes(d + off, bs), cipher_bytes(s + off, bs),
                         cipher_bytes(iv, bs));
        if (x->aes != NULL)
            burrow__aes_encrypt_blocks(x->aes, d + off, d + off, 1);
        else
            cipher_block_encrypt(x->b, cipher_bytes(d + off, bs),
                                 cipher_bytes(d + off, bs));
        /* Move to the next block with this block as the next iv. */
        iv = d + off;
    }
    /* Save the iv for the next CryptBlocks call. */
    if (iv != x->iv)
        memcpy(x->iv, iv, (size_t)bs);
}

/* For each block, the decrypted data is XORed with the previous block's
 * ciphertext. Going backwards means dst can be src: by the time a block is
 * written, nothing still to be decrypted needs it. */
static void cipher_cbc_decrypt_generic(CipherCbc *x, Byte *d, Byte *s, Int n) {
    Int bs = x->block_size;
    Int end = n;
    Int start = end - bs;
    Int prev = start - bs;

    /* Copy the last block of ciphertext in preparation as the new iv. */
    memcpy(x->tmp, s + start, (size_t)bs);

    /* Loop over all but the first block. */
    while (start > 0) {
        cipher_block_decrypt(x->b, cipher_bytes(d + start, bs),
                             cipher_bytes(s + start, bs));
        subtle_xor_bytes(cipher_bytes(d + start, bs), cipher_bytes(d + start, bs),
                         cipher_bytes(s + prev, bs));
        start = prev;
        prev -= bs;
    }

    /* The first block is special because it uses the saved iv. */
    cipher_block_decrypt(x->b, cipher_bytes(d + start, bs),
                         cipher_bytes(s + start, bs));
    subtle_xor_bytes(cipher_bytes(d + start, bs), cipher_bytes(d + start, bs),
                     cipher_bytes(x->iv, bs));

    /* Set the new iv to the last block we copied earlier. */
    Byte *t = x->iv;
    x->iv = x->tmp;
    x->tmp = t;
}

/* The same backwards walk, eight blocks at a time through a buffer, so the AES
 * code can work on several at once. */
static void cipher_cbc_decrypt_aes(CipherCbc *x, Byte *d, const Byte *s, Int n) {
    Byte iv[AES_BLOCK_SIZE];
    memcpy(iv, x->iv, sizeof iv);
    memcpy(x->iv, s + n - AES_BLOCK_SIZE, AES_BLOCK_SIZE);
    Int end = n;
    while (end > 0) {
        Int start = end - (Int)8 * AES_BLOCK_SIZE;
        if (start < 0)
            start = 0;
        Byte buf[8 * AES_BLOCK_SIZE];
        Int len = end - start;
        burrow__aes_decrypt_blocks(x->aes, buf, s + start,
                                   (size_t)(len / AES_BLOCK_SIZE));
        if (start > 0) {
            for (Int i = 0; i < len; i++)
                buf[i] ^= s[start - AES_BLOCK_SIZE + i];
        } else {
            for (Int i = 0; i < AES_BLOCK_SIZE; i++)
                buf[i] ^= iv[i];
            for (Int i = AES_BLOCK_SIZE; i < len; i++)
                buf[i] ^= s[i - AES_BLOCK_SIZE];
        }
        memcpy(d + start, buf, (size_t)len);
        end = start;
    }
}

static void cipher_cbc_decrypt(void *self, Slice dst, Slice src) {
    CipherCbc *x = (CipherCbc *)self;
    cipher_cbc_check(x, dst, src);
    if (src.len == 0)
        return;
    if (x->aes != NULL)
        cipher_cbc_decrypt_aes(x, (Byte *)dst.p, (const Byte *)src.p, src.len);
    else
        cipher_cbc_decrypt_generic(x, (Byte *)dst.p, (Byte *)src.p, src.len);
}

static const CipherBlockModeVT cipher_cbc_enc_vt = {NULL, cipher_cbc_block_size,
                                                    cipher_cbc_encrypt};
static const CipherBlockModeVT cipher_cbc_dec_vt = {NULL, cipher_cbc_block_size,
                                                    cipher_cbc_decrypt};

CipherBlockMode cipher_new_cbc_encrypter(Alloc *a, CipherBlock b, Slice iv) {
    CipherCbc *x = cipher_cbc_make(
        a, b, iv, BURROW_S("cipher.NewCBCEncrypter: IV length must equal block size"));
    if (x == NULL)
        return (CipherBlockMode){NULL, NULL};
    return (CipherBlockMode){&cipher_cbc_enc_vt, x};
}

CipherBlockMode cipher_new_cbc_decrypter(Alloc *a, CipherBlock b, Slice iv) {
    CipherCbc *x = cipher_cbc_make(
        a, b, iv, BURROW_S("cipher.NewCBCDecrypter: IV length must equal block size"));
    if (x == NULL)
        return (CipherBlockMode){NULL, NULL};
    return (CipherBlockMode){&cipher_cbc_dec_vt, x};
}

void cipher_cbc_set_iv(CipherBlockMode m, Slice iv) {
    if (m.vt != &cipher_cbc_enc_vt && m.vt != &cipher_cbc_dec_vt)
        panic_str(BURROW_S("cipher: SetIV on a BlockMode that is not CBC"));
    CipherCbc *x = (CipherCbc *)m.data;
    if (iv.len != x->block_size)
        panic_str(BURROW_S("cipher: incorrect length IV"));
    memcpy(x->iv, iv.p, (size_t)iv.len);
}

/* -------------------------------------------------------------------- CTR */

/* aes.CTR: the counter as two 64 bit halves and how far into the stream the
 * next byte is, so the key stream is made from the offset alone. */
typedef struct CipherAesCtr {
    const AesBlock *b;
    uint64_t ivlo, ivhi;
    uint64_t offset;
} CipherAesCtr;

static uint64_t cipher_be64(const Byte *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
        v = (v << 8) | p[i];
    return v;
}

static void cipher_put_be64(Byte *p, uint64_t v) {
    for (int i = 7; i >= 0; i--) {
        p[i] = (Byte)v;
        v >>= 8;
    }
}

/* n blocks of key stream starting at counter (hi, lo), XORed with src into
 * dst. src is read in full before dst is written. */
static void cipher_aes_ctr_blocks(const AesBlock *b, Byte *dst, const Byte *src,
                                  Int len, uint64_t lo, uint64_t hi) {
    Byte buf[8 * AES_BLOCK_SIZE] = {0};
    Int n = (len + AES_BLOCK_SIZE - 1) / AES_BLOCK_SIZE;
    for (Int i = 0; i < n; i++) {
        cipher_put_be64(buf + 16 * i, hi);
        cipher_put_be64(buf + 16 * i + 8, lo);
        lo++;
        hi += lo == 0;
    }
    burrow__aes_encrypt_blocks(b, buf, buf, (size_t)n);
    for (Int i = 0; i < len; i++)
        buf[i] ^= src[i];
    memcpy(dst, buf, (size_t)len);
}

/* XORKeyStreamAt: the stream from offset, whatever came before. */
static void cipher_aes_ctr_xor_at(CipherAesCtr *c, Byte *dst, const Byte *src, Int len,
                                  uint64_t offset) {
    uint64_t q = offset / AES_BLOCK_SIZE;
    uint64_t lo = c->ivlo + q;
    uint64_t hi = c->ivhi + (lo < q);

    Int block_offset = (Int)(offset % AES_BLOCK_SIZE);
    if (block_offset != 0) {
        /* We have a partial block at the beginning. */
        Byte in[AES_BLOCK_SIZE] = {0};
        Byte out[AES_BLOCK_SIZE];
        Int n = AES_BLOCK_SIZE - block_offset;
        if (n > len)
            n = len;
        memcpy(in + block_offset, src, (size_t)n);
        cipher_aes_ctr_blocks(c->b, out, in, AES_BLOCK_SIZE, lo, hi);
        memcpy(dst, out + block_offset, (size_t)n);
        src += n;
        dst += n;
        len -= n;
        lo++;
        hi += lo == 0;
    }

    while (len > 0) {
        Int n = len < (Int)8 * AES_BLOCK_SIZE ? len : (Int)8 * AES_BLOCK_SIZE;
        cipher_aes_ctr_blocks(c->b, dst, src, n, lo, hi);
        src += n;
        dst += n;
        len -= n;
        lo += 8;
        hi += lo < 8;
    }
}

static void cipher_aes_ctr_xor(void *self, Slice dst, Slice src) {
    CipherAesCtr *c = (CipherAesCtr *)self;
    if (dst.len < src.len)
        panic_str(BURROW_S("crypto/aes: len(dst) < len(src)"));
    if (cipher_inexact_overlap(dst.p, src.p, src.len))
        panic_str(BURROW_S("crypto/aes: invalid buffer overlap"));
    cipher_aes_ctr_xor_at(c, (Byte *)dst.p, (const Byte *)src.p, src.len, c->offset);
    uint64_t next = c->offset + (uint64_t)src.len;
    if (next < c->offset)
        panic_str(BURROW_S("crypto/aes: counter overflow"));
    c->offset = next;
}

static const CipherStreamVT cipher_aes_ctr_vt = {NULL, cipher_aes_ctr_xor};

bool burrow__aes_ctr_xor_key_stream_at(CipherStream s, Slice dst, Slice src,
                                       uint64_t offset) {
    if (s.vt != &cipher_aes_ctr_vt)
        return false;
    if (dst.len < src.len)
        panic_str(BURROW_S("crypto/aes: len(dst) < len(src)"));
    if (cipher_inexact_overlap(dst.p, src.p, src.len))
        panic_str(BURROW_S("crypto/aes: invalid buffer overlap"));
    cipher_aes_ctr_xor_at((CipherAesCtr *)s.data, (Byte *)dst.p, (const Byte *)src.p,
                          src.len, offset);
    return true;
}

/* The general CTR, which keeps up to 512 bytes of key stream ahead. */
typedef struct CipherCtr {
    CipherBlock b;
    Int block_size;
    Byte *ctr;
    Byte *out;
    Int out_len;
    Int out_cap;
    Int out_used;
} CipherCtr;

static void cipher_ctr_refill(CipherCtr *x) {
    Int remain = x->out_len - x->out_used;
    memmove(x->out, x->out + x->out_used, (size_t)remain);
    Int bs = x->block_size;
    while (remain <= x->out_cap - bs) {
        cipher_block_encrypt(x->b, cipher_bytes(x->out + remain, bs),
                             cipher_bytes(x->ctr, bs));
        remain += bs;
        /* Increment counter. */
        for (Int i = bs - 1; i >= 0; i--) {
            x->ctr[i]++;
            if (x->ctr[i] != 0)
                break;
        }
    }
    x->out_len = remain;
    x->out_used = 0;
}

static void cipher_ctr_xor(void *self, Slice dst, Slice src) {
    CipherCtr *x = (CipherCtr *)self;
    if (dst.len < src.len)
        panic_str(BURROW_S("crypto/cipher: output smaller than input"));
    if (cipher_inexact_overlap(dst.p, src.p, src.len))
        panic_str(BURROW_S("crypto/cipher: invalid buffer overlap"));
    Byte *d = (Byte *)dst.p;
    Byte *s = (Byte *)src.p;
    Int len = src.len;
    while (len > 0) {
        if (x->out_used >= x->out_len - x->block_size)
            cipher_ctr_refill(x);
        Int n = subtle_xor_bytes(
            cipher_bytes(d, len), cipher_bytes(s, len),
            cipher_bytes(x->out + x->out_used, x->out_len - x->out_used));
        d += n;
        s += n;
        len -= n;
        x->out_used += n;
    }
}

static const CipherStreamVT cipher_ctr_vt = {NULL, cipher_ctr_xor};

CipherStream cipher_new_ctr(Alloc *a, CipherBlock b, Slice iv) {
    const AesBlock *ab = burrow__aes_block_of(b);
    if (ab != NULL) {
        if (iv.len != AES_BLOCK_SIZE)
            panic_str(BURROW_S("bad IV length"));
        CipherAesCtr *c = BURROW_NEW(a, CipherAesCtr);
        if (c == NULL)
            return (CipherStream){NULL, NULL};
        c->b = ab;
        c->ivhi = cipher_be64((const Byte *)iv.p);
        c->ivlo = cipher_be64((const Byte *)iv.p + 8);
        return (CipherStream){&cipher_aes_ctr_vt, c};
    }
    Int bs = cipher_block_block_size(b);
    if (iv.len != bs)
        panic_str(BURROW_S("cipher.NewCTR: IV length must equal block size"));
    Int cap = CIPHER_STREAM_BUFFER < bs ? bs : CIPHER_STREAM_BUFFER;
    CipherCtr *x = (CipherCtr *)mem_alloc(a, sizeof(CipherCtr) + (size_t)(bs + cap),
                                          _Alignof(CipherCtr));
    if (x == NULL)
        return (CipherStream){NULL, NULL};
    x->b = b;
    x->block_size = bs;
    x->ctr = (Byte *)(x + 1);
    x->out = x->ctr + bs;
    x->out_cap = cap;
    memcpy(x->ctr, iv.p, (size_t)bs);
    return (CipherStream){&cipher_ctr_vt, x};
}

/* -------------------------------------------------------------------- OFB */

typedef struct CipherOfb {
    CipherBlock b;
    Int block_size;
    Byte *cipher;
    Byte *out;
    Int out_len;
    Int out_cap;
    Int out_used;
} CipherOfb;

static void cipher_ofb_refill(CipherOfb *x) {
    Int bs = x->block_size;
    Int remain = x->out_len - x->out_used;
    if (remain > x->out_used)
        return;
    memmove(x->out, x->out + x->out_used, (size_t)remain);
    while (remain < x->out_cap - bs) {
        cipher_block_encrypt(x->b, cipher_bytes(x->cipher, bs),
                             cipher_bytes(x->cipher, bs));
        memcpy(x->out + remain, x->cipher, (size_t)bs);
        remain += bs;
    }
    x->out_len = remain;
    x->out_used = 0;
}

static void cipher_ofb_xor(void *self, Slice dst, Slice src) {
    CipherOfb *x = (CipherOfb *)self;
    if (dst.len < src.len)
        panic_str(BURROW_S("crypto/cipher: output smaller than input"));
    if (cipher_inexact_overlap(dst.p, src.p, src.len))
        panic_str(BURROW_S("crypto/cipher: invalid buffer overlap"));
    Byte *d = (Byte *)dst.p;
    Byte *s = (Byte *)src.p;
    Int len = src.len;
    while (len > 0) {
        if (x->out_used >= x->out_len - x->block_size)
            cipher_ofb_refill(x);
        Int n = subtle_xor_bytes(
            cipher_bytes(d, len), cipher_bytes(s, len),
            cipher_bytes(x->out + x->out_used, x->out_len - x->out_used));
        d += n;
        s += n;
        len -= n;
        x->out_used += n;
    }
}

static const CipherStreamVT cipher_ofb_vt = {NULL, cipher_ofb_xor};

CipherStream cipher_new_ofb(Alloc *a, CipherBlock b, Slice iv) {
    Int bs = cipher_block_block_size(b);
    if (iv.len != bs)
        panic_str(BURROW_S("cipher.NewOFB: IV length must equal block size"));
    Int cap = CIPHER_STREAM_BUFFER < bs ? bs : CIPHER_STREAM_BUFFER;
    CipherOfb *x = (CipherOfb *)mem_alloc(a, sizeof(CipherOfb) + (size_t)(bs + cap),
                                          _Alignof(CipherOfb));
    if (x == NULL)
        return (CipherStream){NULL, NULL};
    x->b = b;
    x->block_size = bs;
    x->cipher = (Byte *)(x + 1);
    x->out = x->cipher + bs;
    x->out_cap = cap;
    memcpy(x->cipher, iv.p, (size_t)bs);
    return (CipherStream){&cipher_ofb_vt, x};
}

/* -------------------------------------------------------------------- CFB */

typedef struct CipherCfb {
    CipherBlock b;
    Int block_size;
    Byte *next;
    Byte *out;
    Int out_used;
    bool decrypt;
} CipherCfb;

static void cipher_cfb_xor(void *self, Slice dst, Slice src) {
    CipherCfb *x = (CipherCfb *)self;
    if (dst.len < src.len)
        panic_str(BURROW_S("crypto/cipher: output smaller than input"));
    if (cipher_inexact_overlap(dst.p, src.p, src.len))
        panic_str(BURROW_S("crypto/cipher: invalid buffer overlap"));
    Int bs = x->block_size;
    Byte *d = (Byte *)dst.p;
    Byte *s = (Byte *)src.p;
    Int len = src.len;
    while (len > 0) {
        if (x->out_used == bs) {
            cipher_block_encrypt(x->b, cipher_bytes(x->out, bs),
                                 cipher_bytes(x->next, bs));
            x->out_used = 0;
        }
        Int n = bs - x->out_used < len ? bs - x->out_used : len;
        if (x->decrypt) {
            /* We can precompute a larger segment of the keystream on
             * decryption. This will allow larger batches for xor, and we
             * should be able to match CTR/OFB performance. */
            memmove(x->next + x->out_used, s, (size_t)n);
        }
        subtle_xor_bytes(cipher_bytes(d, n), cipher_bytes(s, n),
                         cipher_bytes(x->out + x->out_used, n));
        if (!x->decrypt)
            memcpy(x->next + x->out_used, d, (size_t)n);
        d += n;
        s += n;
        len -= n;
        x->out_used += n;
    }
}

static const CipherStreamVT cipher_cfb_vt = {NULL, cipher_cfb_xor};

static CipherStream cipher_new_cfb(Alloc *a, CipherBlock b, Slice iv, bool decrypt) {
    Int bs = cipher_block_block_size(b);
    if (iv.len != bs) {
        /* Stack trace will indicate whether it was de- or en-cryption. */
        panic_str(BURROW_S("cipher.newCFB: IV length must equal block size"));
    }
    CipherCfb *x = (CipherCfb *)mem_alloc(a, sizeof(CipherCfb) + 2 * (size_t)bs,
                                          _Alignof(CipherCfb));
    if (x == NULL)
        return (CipherStream){NULL, NULL};
    x->b = b;
    x->block_size = bs;
    x->next = (Byte *)(x + 1);
    x->out = x->next + bs;
    x->out_used = bs;
    x->decrypt = decrypt;
    memcpy(x->next, iv.p, (size_t)bs);
    return (CipherStream){&cipher_cfb_vt, x};
}

CipherStream cipher_new_cfb_encrypter(Alloc *a, CipherBlock b, Slice iv) {
    return cipher_new_cfb(a, b, iv, false);
}

CipherStream cipher_new_cfb_decrypter(Alloc *a, CipherBlock b, Slice iv) {
    return cipher_new_cfb(a, b, iv, true);
}

/* ----------------------------------------------------------------- io */

Int cipher_stream_reader_read(CipherStreamReader r, Slice dst, Error *err) {
    Int n = r.r.vt->read(r.r.data, dst, err);
    if (n > 0) {
        Slice got = slice_from(dst.p, n, n, TYPE_BYTE);
        cipher_stream_xor_key_stream(r.s, got, got);
    }
    return n;
}

static Int cipher_stream_reader_vt_read(void *self, Slice p, Error *err) {
    return cipher_stream_reader_read(*(const CipherStreamReader *)self, p, err);
}

static const IoReaderVT cipher_stream_reader_vt = {NULL, cipher_stream_reader_vt_read};

IoReader cipher_stream_reader_as_io_reader(CipherStreamReader *r) {
    return (IoReader){&cipher_stream_reader_vt, r};
}

Int cipher_stream_writer_write(CipherStreamWriter w, Slice src, Error *err) {
    /* Go makes a new slice for every write. A small one fits on the stack. */
    Byte small[512];
    Byte *c = small;
    Alloc *h = heap_allocator();
    if (src.len > (Int)sizeof small) {
        c = (Byte *)mem_alloc_nozero(h, (size_t)src.len, 1);
        if (c == NULL) {
            BURROW_OUT(err, burrow_err_out_of_memory);
            return 0;
        }
    }
    Slice cs = slice_from(c, src.len, src.len, TYPE_BYTE);
    cipher_stream_xor_key_stream(w.s, cs, src);
    Error e = BURROW_NO_ERROR;
    Int n = w.w.vt->write(w.w.data, cs, &e);
    if (n != src.len && !BURROW_FAILED(e)) /* should never happen */
        e = io_err_short_write;
    if (c != small)
        mem_free(h, c, (size_t)src.len, 1);
    BURROW_OUT(err, e);
    return n;
}

Error cipher_stream_writer_close(CipherStreamWriter w) {
    if (w.closer.vt != NULL)
        return w.closer.vt->close(w.closer.data);
    return BURROW_NO_ERROR;
}

static Int cipher_stream_writer_vt_write(void *self, Slice p, Error *err) {
    return cipher_stream_writer_write(*(const CipherStreamWriter *)self, p, err);
}

static const IoWriterVT cipher_stream_writer_vt = {NULL, cipher_stream_writer_vt_write};

IoWriter cipher_stream_writer_as_io_writer(CipherStreamWriter *w) {
    return (IoWriter){&cipher_stream_writer_vt, w};
}
