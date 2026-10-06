/* Derived from Go's src/crypto/rc4/rc4.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/crypto/rc4.h"

#include "burrow/core.h"
#include "burrow/crypto/cipher.h"
#include "burrow/error.h"
#include "burrow/mem.h"
#include "burrow/panic.h"
#include "burrow/runtime.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include <stdint.h>
#include <string.h>

/* ---------------------------------------------------------- KeySizeError */

/* The length first, so the data pointer of the Error is a pointer to an
 * Rc4KeySizeError and errors_as hands it straight back. */
typedef struct Rc4KeySizeBox {
    Rc4KeySizeError k;
    Str message;
} Rc4KeySizeBox;

static const Type rc4_key_size_desc = {
    {(const Byte *)"KeySizeError", 12},
    {(const Byte *)"crypto/rc4", 10},
    KIND_INT,
    (uint32_t)sizeof(Rc4KeySizeError),
    (uint16_t)_Alignof(Rc4KeySizeError),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x72636b73U, /* "rcks" */
    NULL,
};

const Type *const TYPE_RC4_KEY_SIZE_ERROR = &rc4_key_size_desc;

static const char rc4_key_size_prefix[] = "crypto/rc4: invalid key size ";

#define RC4_PREFIX_LEN ((Int)sizeof(rc4_key_size_prefix) - 1)

/* Writes the message for k to p, or only counts it when p is NULL, and returns
 * its length. */
static Int rc4_key_size_message(Byte *p, Rc4KeySizeError k) {
    Byte digits[20];
    Int n = 0;
    uint64_t u = k < 0 ? (uint64_t)0 - (uint64_t)k : (uint64_t)k;
    do {
        digits[n++] = (Byte)('0' + (int)(u % 10));
        u /= 10;
    } while (u != 0);
    Int len = RC4_PREFIX_LEN + (k < 0) + n;
    if (p != NULL) {
        memcpy(p, rc4_key_size_prefix, (size_t)RC4_PREFIX_LEN);
        p += RC4_PREFIX_LEN;
        if (k < 0)
            *p++ = '-';
        while (n > 0)
            *p++ = digits[--n];
    }
    return len;
}

static Str rc4_key_size_text(const void *self) {
    return ((const Rc4KeySizeBox *)self)->message;
}

static Error rc4_key_size_clone(const void *self, Alloc *a) {
    return rc4_key_size_error_as_error(((const Rc4KeySizeBox *)self)->k, a);
}

static const ErrorVT rc4_key_size_vt = {
    .self_type = &rc4_key_size_desc,
    .message = rc4_key_size_text,
    .clone = rc4_key_size_clone,
};

Error rc4_key_size_error_as_error(Rc4KeySizeError k, Alloc *a) {
    Int mlen = rc4_key_size_message(NULL, k);
    Rc4KeySizeBox *b = (Rc4KeySizeBox *)mem_alloc_nozero(
        a, sizeof(Rc4KeySizeBox) + (size_t)mlen, _Alignof(Rc4KeySizeBox));
    if (b == NULL)
        return burrow_err_out_of_memory;
    Byte *p = (Byte *)(b + 1);
    rc4_key_size_message(p, k);
    b->k = k;
    b->message = str_from_bytes(p, mlen);
    return (Error){&rc4_key_size_vt, b};
}

Str rc4_key_size_error_error(Rc4KeySizeError k, Alloc *a) {
    Int mlen = rc4_key_size_message(NULL, k);
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)mlen, 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    rc4_key_size_message(p, k);
    return str_from_bytes(p, mlen);
}

/* ---------------------------------------------------------------- Cipher */

Rc4Cipher *rc4_new_cipher(Alloc *a, Slice key, Error *err) {
    Int k = key.len;
    if (k < 1 || k > 256) {
        BURROW_OUT(err, rc4_key_size_error_as_error(k, error_allocator()));
        return NULL;
    }
    Rc4Cipher *c = BURROW_NEW(a, Rc4Cipher);
    if (c == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    const Byte *kb = (const Byte *)key.p;
    for (int i = 0; i < 256; i++)
        c->s[i] = (uint32_t)i;
    uint8_t j = 0;
    for (int i = 0; i < 256; i++) {
        j = (uint8_t)(j + (uint8_t)c->s[i] + kb[i % k]);
        uint32_t t = c->s[i];
        c->s[i] = c->s[j];
        c->s[j] = t;
    }
    c->i = 0;
    c->j = 0;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return c;
}

void rc4_cipher_reset(Rc4Cipher *c) {
    memset(c->s, 0, sizeof c->s);
    c->i = 0;
    c->j = 0;
}

/* Go's alias.InexactOverlap on two runs of n bytes. */
static bool rc4_inexact_overlap(const void *x, const void *y, Int n) {
    uintptr_t a = (uintptr_t)x;
    uintptr_t b = (uintptr_t)y;
    if (a == b)
        return false;
    return a <= b + (uintptr_t)(n - 1) && b <= a + (uintptr_t)(n - 1);
}

void rc4_cipher_xor_key_stream(Rc4Cipher *c, Slice dst, Slice src) {
    if (src.len == 0)
        return;
    /* Go's _ = dst[len(src)-1]. */
    if (dst.len < src.len)
        runtime_index_out_of_range(src.len - 1, dst.len);
    if (rc4_inexact_overlap(dst.p, src.p, src.len))
        panic_str(BURROW_S("crypto/rc4: invalid buffer overlap"));
    uint8_t i = c->i, j = c->j;
    Byte *d = (Byte *)dst.p;
    const Byte *s = (const Byte *)src.p;
    for (Int k = 0; k < src.len; k++) {
        i++;
        uint32_t x = c->s[i];
        j = (uint8_t)(j + (uint8_t)x);
        uint32_t y = c->s[j];
        c->s[i] = y;
        c->s[j] = x;
        d[k] = (Byte)(s[k] ^ (uint8_t)c->s[(uint8_t)(x + y)]);
    }
    c->i = i;
    c->j = j;
}

static void rc4_stream_xor(void *self, Slice dst, Slice src) {
    rc4_cipher_xor_key_stream((Rc4Cipher *)self, dst, src);
}

static const CipherStreamVT rc4_stream_vt = {NULL, rc4_stream_xor};

CipherStream rc4_cipher_as_cipher_stream(Rc4Cipher *c) {
    return (CipherStream){&rc4_stream_vt, c};
}
