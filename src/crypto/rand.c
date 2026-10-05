/* Derived from Go's src/crypto/rand/rand.go, util.go and text.go, and
 * src/crypto/internal/rand/rand.go and src/crypto/internal/randutil.
 * Go source: go1.27.1.
 *
 * Copyright 2010 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/crypto/rand.h"

#include "burrow/atomic.h"
#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/math/big.h"
#include "burrow/mem.h"
#include "burrow/mem/heap.h"
#include "burrow/pal.h"
#include "burrow/panic.h"
#include "burrow/runtime.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include "rand_internal.h"

#include <stdint.h>
#include <string.h>

/* ---------------------------------------------------------------- GODEBUG */

enum {
    CRAND_DEBUG_KNOWN = 1 << 0,
    CRAND_DEBUG_CUSTOM = 1 << 1, /* cryptocustomrand=1 */
};

static uint32_t crypto_rand_debug_flags;

/* The value of key in GODEBUG, the last one when it is there twice. */
static bool crand_godebug(const char *env, const char *key, Str *val) {
    size_t kl = strlen(key);
    bool found = false;
    const char *p = env;
    while (*p != '\0') {
        const char *end = strchr(p, ',');
        if (end == NULL)
            end = p + strlen(p);
        if ((size_t)(end - p) > kl && memcmp(p, key, kl) == 0 && p[kl] == '=') {
            *val = str_from_bytes(p + kl + 1, (Int)(end - p - (ptrdiff_t)kl - 1));
            found = true;
        }
        p = *end == ',' ? end + 1 : end;
    }
    return found;
}

static uint32_t crand_debug_parse(const char *v) {
    uint32_t f = CRAND_DEBUG_KNOWN;
    Str s;
    if (v != NULL && crand_godebug(v, "cryptocustomrand", &s) && s.len == 1 &&
        s.p[0] == '1')
        f |= CRAND_DEBUG_CUSTOM;
    burrow__atomic_store_relaxed_u32(&crypto_rand_debug_flags, f);
    return f;
}

static uint32_t crand_debug_load(void) {
    uint32_t f = burrow__atomic_load_relaxed_u32(&crypto_rand_debug_flags);
    if ((f & CRAND_DEBUG_KNOWN) != 0)
        return f;
    const char *v = NULL;
    for (const char *const *env = pal_environ(); env != NULL && *env != NULL; env++) {
        if (strncmp(*env, "GODEBUG=", 8) == 0) {
            v = *env + 8;
            break;
        }
    }
    return crand_debug_parse(v);
}

void burrow__crypto_rand_godebug_set(const char *value) {
    if (value == NULL)
        burrow__atomic_store_relaxed_u32(&crypto_rand_debug_flags, 0);
    else
        (void)crand_debug_parse(value);
}

/* ------------------------------------------------------------------ Reader */

/* Go's fatal, which is runtime.throw: the message, and the process ends. */
BURROW_NORETURN static void crand_fatal(const char *why, Int why_len) {
    static const char head[] =
        "crypto/rand: failed to read random data (see https://go.dev/issue/66821): ";
    char msg[256];
    size_t hl = sizeof head - 1;
    size_t wl = (size_t)why_len;
    if (wl > sizeof msg - hl)
        wl = sizeof msg - hl;
    memcpy(msg, head, hl);
    if (wl > 0)
        memcpy(msg + hl, why, wl);
    runtime_throw(str_from_bytes(msg, (Int)(hl + wl)));
}

/* The system generator, which never comes back short: it fills p or the
 * process ends. */
static void crand_system(Slice p) {
    PalErrno e = PAL_OK;
    if (!pal_random_bytes(p.p, p.len, &e)) {
        const char *why = pal_errno_string(e);
        crand_fatal(why, (Int)strlen(why));
    }
}

static Int crand_default_read(void *self, Slice p, Error *err) {
    (void)self;
    crand_system(p);
    BURROW_OUT(err, BURROW_NO_ERROR);
    return p.len;
}

static const IoReaderVT crand_default_vt = {NULL, crand_default_read};

/* crypto/internal/rand.Reader, which is what crypto_rand_reader starts as and
 * what Prime reads from whatever crypto_rand_reader has become. */
static const IoReader crand_default_reader = {&crand_default_vt, NULL};

IoReader crypto_rand_reader = {&crand_default_vt, NULL};

bool burrow__crypto_rand_is_default_reader(IoReader r) {
    return r.vt == &crand_default_vt;
}

Int crypto_rand_read(Slice b, Error *err) {
    if (burrow__crypto_rand_is_default_reader(crypto_rand_reader)) {
        crand_system(b);
    } else {
        Error e = BURROW_NO_ERROR;
        (void)io_read_full(crypto_rand_reader, b, &e);
        if (BURROW_FAILED(e)) {
            Str why = error_text(e);
            crand_fatal((const char *)why.p, why.len);
        }
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return b.len;
}

/* -------------------------------------------------------------------- Text */

static const char crand_base32_alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

Str crypto_rand_text(Alloc *a) {
    /* ⌈log₃₂ 2¹²⁸⌉ = 26 chars */
    Byte src[26];
    crypto_rand_read(slice_from(src, 26, 26, TYPE_BYTE), NULL);
    for (size_t i = 0; i < sizeof src; i++)
        src[i] = (Byte)crand_base32_alphabet[src[i] % 32];
    return str_clone(a, str_from_bytes((const char *)src, 26));
}

/* ------------------------------------------------------------- Int, Prime */

/* randutil.MaybeReadByte: half the time, one byte from r and thrown away, so
 * that a caller cannot come to depend on exactly what the function reads. */
static void crand_maybe_read_byte(IoReader r) {
    if ((runtime_rand64() & 1) == 1)
        return;
    Byte buf[1];
    (void)r.vt->read(r.data, slice_from(buf, 1, 1, TYPE_BYTE), NULL);
}

/* crypto/internal/rand.CustomReader: the system generator, unless GODEBUG has
 * cryptocustomrand=1. Go also passes over its testing reader here, which
 * testing/cryptotest sets and burrow does not have. */
static IoReader crand_custom_reader(IoReader r) {
    if ((crand_debug_load() & CRAND_DEBUG_CUSTOM) != 0) {
        if (!burrow__crypto_rand_is_default_reader(r))
            crand_maybe_read_byte(r);
        return r;
    }
    return crand_default_reader;
}

static void crand_drop(Alloc *a, BigInt *n, Slice bytes) {
    Alloc *h = a != NULL ? a : heap_allocator();
    if (n != NULL) {
        big_int_free(n);
        mem_free(h, n, sizeof(BigInt), _Alignof(BigInt));
    }
    if (bytes.p != NULL)
        mem_free(h, bytes.p, (size_t)bytes.cap, 1);
}

BigInt *crypto_rand_prime(Alloc *a, IoReader rand, Int bits, Error *err) {
    if (bits < 2) {
        BURROW_OUT(
            err,
            errors_new(error_allocator(),
                       BURROW_S("crypto/rand: prime size must be at least 2-bit")));
        return NULL;
    }

    rand = crand_custom_reader(rand);

    unsigned b = (unsigned)(bits % 8);
    if (b == 0)
        b = 8;

    Alloc *h = a != NULL ? a : heap_allocator();
    Int len = (bits + 7) / 8;
    Slice bytes = slice_make(h, TYPE_BYTE, len, len);
    if (bytes.p == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    Byte *p8 = (Byte *)bytes.p;
    BigInt *p = big_new_int(a, 0);

    for (;;) {
        Error e = BURROW_NO_ERROR;
        (void)io_read_full(rand, bytes, &e);
        if (BURROW_FAILED(e)) {
            crand_drop(a, p, bytes);
            BURROW_OUT(err, e);
            return NULL;
        }

        /* Clear bits in the first byte to make sure the candidate has a size
         * <= bits. */
        p8[0] &= (Byte)((1u << b) - 1);
        /* Don't let the value be too small, i.e, set the most significant two
         * bits. Setting the top two bits, rather than just the top bit, means
         * that when two of these values are multiplied together, the result
         * isn't ever one bit short. */
        if (b >= 2) {
            p8[0] |= (Byte)(3u << (b - 2));
        } else {
            /* Here b==1, because b cannot be zero. */
            p8[0] |= 1;
            if (len > 1)
                p8[1] |= 0x80;
        }
        /* Make the value odd since an even number this large certainly isn't
         * prime. */
        p8[len - 1] |= 1;

        big_int_set_bytes(p, bytes);
        if (big_int_probably_prime(p, 20)) {
            crand_drop(a, NULL, bytes);
            BURROW_OUT(err, BURROW_NO_ERROR);
            return p;
        }
    }
}

BigInt *crypto_rand_int(Alloc *a, IoReader rand, const BigInt *max, Error *err) {
    if (big_int_sign(max) <= 0)
        panic_str(BURROW_S("crypto/rand: argument to Int is <= 0"));
    BigInt *n = big_new_int(a, 0);
    big_int_sub(n, max, big_int_set_uint64(n, 1));
    /* bitLen is the maximum bit length needed to encode a value < max. */
    Int bit_len = big_int_bit_len(n);
    if (bit_len == 0) {
        /* the only valid result is 0 */
        BURROW_OUT(err, BURROW_NO_ERROR);
        return n;
    }
    /* k is the maximum byte length needed to encode a value < max. */
    Int k = (bit_len + 7) / 8;
    /* b is the number of bits in the most significant byte of max-1. */
    unsigned b = (unsigned)(bit_len % 8);
    if (b == 0)
        b = 8;

    Alloc *h = a != NULL ? a : heap_allocator();
    Slice bytes = slice_make(h, TYPE_BYTE, k, k);
    if (bytes.p == NULL) {
        crand_drop(a, n, bytes);
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }

    for (;;) {
        Error e = BURROW_NO_ERROR;
        (void)io_read_full(rand, bytes, &e);
        if (BURROW_FAILED(e)) {
            crand_drop(a, n, bytes);
            BURROW_OUT(err, e);
            return NULL;
        }

        /* Clear bits in the first byte to increase the probability that the
         * candidate is < max. */
        ((Byte *)bytes.p)[0] &= (Byte)((1u << b) - 1);

        big_int_set_bytes(n, bytes);
        if (big_int_cmp(n, max) < 0) {
            crand_drop(a, NULL, bytes);
            BURROW_OUT(err, BURROW_NO_ERROR);
            return n;
        }
    }
}
