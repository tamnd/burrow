/* crypto/tls: the builder that tls_handshake_messages.c and tls_ticket.c lay
 * messages out with, in place of Go's cryptobyte.Builder.
 *
 * Go builds most messages with a cryptobyte.Builder and a continuation for
 * each length prefix. Here a small builder keeps the open prefixes on a stack
 * instead: tlsb_begin writes a placeholder, tlsb_end fills the length in, and
 * the first error sticks the way a Builder's does, with the same text. The
 * nesting of begin and end follows the nesting of Go's closures line for line.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_CRYPTO_TLS_BUILDER_H
#define BURROW_CRYPTO_TLS_BUILDER_H

#include "burrow/crypto/tls.h"

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/mem.h"
#include "burrow/panic.h"
#include "burrow/slice.h"

#include "cryptobyte.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* The deepest any message nests its length prefixes. */
#define TLSB_MAX_DEPTH 8

typedef struct TlsBuilder {
    Alloc *a;
    Byte *p;
    Int len;
    Int cap;
    Int start[TLSB_MAX_DEPTH]; /* where each open prefix's contents begin */
    int len_len[TLSB_MAX_DEPTH];
    int depth;
    bool failed;
    Error err;
} TlsBuilder;

static inline void tlsb_init(TlsBuilder *b, Alloc *a) {
    memset(b, 0, sizeof(*b));
    b->a = a;
    b->err = BURROW_NO_ERROR;
}

static inline void tlsb_fail(TlsBuilder *b, Error err) {
    if (!b->failed) {
        b->failed = true;
        b->err = err;
    }
}

/* Room for n more bytes. False when the builder has already failed or a is out
 * of memory. */
static inline bool tlsb_room(TlsBuilder *b, Int n) {
    if (b->failed)
        return false;
    if (b->cap - b->len >= n)
        return true;
    Int ncap = b->cap < 64 ? 64 : b->cap;
    while (ncap - b->len < n)
        ncap *= 2;
    Byte *np = (Byte *)mem_realloc(b->a, b->p, (size_t)b->cap, (size_t)ncap, 1);
    if (np == NULL) {
        tlsb_fail(b, burrow_err_out_of_memory);
        return false;
    }
    b->p = np;
    b->cap = ncap;
    return true;
}

static inline void tlsb_add(TlsBuilder *b, const void *v, Int n) {
    if (n <= 0 || !tlsb_room(b, n))
        return;
    memcpy(b->p + b->len, v, (size_t)n);
    b->len += n;
}

static inline void tlsb_u8(TlsBuilder *b, uint8_t v) {
    tlsb_add(b, &v, 1);
}

static inline void tlsb_u16(TlsBuilder *b, uint16_t v) {
    Byte x[2] = {(Byte)(v >> 8), (Byte)v};
    tlsb_add(b, x, 2);
}

static inline void tlsb_u32(TlsBuilder *b, uint32_t v) {
    Byte x[4] = {(Byte)(v >> 24), (Byte)(v >> 16), (Byte)(v >> 8), (Byte)v};
    tlsb_add(b, x, 4);
}

/* addUint64 appends a big-endian, 64-bit value to the cryptobyte.Builder. */
static inline void tlsb_u64(TlsBuilder *b, uint64_t v) {
    tlsb_u32(b, (uint32_t)(v >> 32));
    tlsb_u32(b, (uint32_t)v);
}

static inline void tlsb_bytes(TlsBuilder *b, Slice v) {
    tlsb_add(b, v.p, v.len);
}

static inline void tlsb_str(TlsBuilder *b, Str v) {
    tlsb_add(b, v.p, v.len);
}

/* Each uint16 in v, a []uint16. */
static inline void tlsb_u16s(TlsBuilder *b, Slice v) {
    const uint16_t *p = (const uint16_t *)v.p;
    for (Int i = 0; i < v.len; i++)
        tlsb_u16(b, p[i]);
}

/* addBytesWithLength appends a sequence of bytes to the builder. If the length
 * of the sequence is not the value specified, it produces an error. */
static inline void tlsb_bytes_with_length(TlsBuilder *b, Slice v, Int n) {
    if (v.len != n) {
        tlsb_fail(b,
                  fmt_errorf_v("invalid value length: expected %d, got %d", n, v.len));
        return;
    }
    tlsb_bytes(b, v);
}

/* Opens a length prefix of len_len bytes. */
static inline void tlsb_begin(TlsBuilder *b, int len_len) {
    if (b->depth == TLSB_MAX_DEPTH)
        panic_str(BURROW_S("tls: internal error: length prefixes nested too deep"));
    static const Byte zero[4] = {0};
    tlsb_add(b, zero, len_len);
    b->start[b->depth] = b->len;
    b->len_len[b->depth] = len_len;
    b->depth++;
}

/* Closes the innermost length prefix and writes its length. */
static inline void tlsb_end(TlsBuilder *b) {
    b->depth--;
    if (b->failed)
        return;
    Int start = b->start[b->depth];
    int len_len = b->len_len[b->depth];
    Int length = b->len - start;
    uint64_t l = (uint64_t)length;
    for (int i = len_len - 1; i >= 0; i--) {
        b->p[start - len_len + i] = (Byte)l;
        l >>= 8;
    }
    if (l != 0)
        tlsb_fail(b, fmt_errorf_v("cryptobyte: pending child length %d exceeds "
                                  "%d-byte length prefix",
                                  length, (Int)len_len));
}

/* The bytes, from a, or a nil slice and the error. */
static inline Slice tlsb_finish(TlsBuilder *b, Error *err) {
    if (b->failed) {
        mem_free(b->a, b->p, (size_t)b->cap, 1);
        BURROW_OUT(err, b->err);
        return slice_nil(TYPE_BYTE);
    }
    Slice out = {b->p, b->len, b->cap, TYPE_BYTE};
    return out;
}

/* marshalCertificate and unmarshalCertificate, the certificate_list of a TLS
 * 1.3 Certificate message, which SessionState.Bytes uses too. unmarshal keeps
 * slices of s and makes the lists in a. */
void burrow__tls_marshal_certificate(TlsBuilder *b, const TlsCertificate *certificate);
bool burrow__tls_unmarshal_certificate(CryptobyteString *s, Alloc *a,
                                       TlsCertificate *certificate);

#endif /* BURROW_CRYPTO_TLS_BUILDER_H */
