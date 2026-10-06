/* Derived from Go's src/crypto/internal/fips140/edwards25519/scalar.go.
 * Go source: go1.27.1.
 *
 * Go's scalar_fiat.go is fiat-crypto's output for Go. This runs on the same
 * arithmetic from fiat-crypto's C output instead, in fiat_25519_scalar_64.h.
 * Compilers with no 128 bit integer get fiat_25519_scalar_32.h, which works in
 * eight 32 bit words. Both have R = 2^256, so a scalar in eight 32 bit words is
 * the one in four 64 bit words cut in half, and Edwards25519Scalar keeps the four
 * either way, so that the constants below and anything that looks inside a
 * scalar read the same on every machine.
 *
 * Copyright 2016 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "edwards25519.h"

#include "burrow/error.h"
#include "burrow/math/bits.h"
#include "burrow/panic.h"
#include "burrow/slice.h"

#include <stdint.h>
#include <string.h>

#if defined(__SIZEOF_INT128__)

#include "fiat_25519_scalar_64.h"

static void sc_fiat_mul(uint64_t out[4], const uint64_t x[4], const uint64_t y[4]) {
    fiat_25519_scalar_mul(out, x, y);
}

static void sc_fiat_add(uint64_t out[4], const uint64_t x[4], const uint64_t y[4]) {
    fiat_25519_scalar_add(out, x, y);
}

static void sc_fiat_sub(uint64_t out[4], const uint64_t x[4], const uint64_t y[4]) {
    fiat_25519_scalar_sub(out, x, y);
}

static void sc_fiat_opp(uint64_t out[4], const uint64_t x[4]) {
    fiat_25519_scalar_opp(out, x);
}

/* Nonzero exactly when x is, folded to its low bit by the caller. */
static uint64_t sc_fiat_nonzero(const uint64_t x[4]) {
    uint64_t out;
    fiat_25519_scalar_nonzero(&out, x);
    return out;
}

static void sc_fiat_from_montgomery(uint64_t out[4], const uint64_t x[4]) {
    fiat_25519_scalar_from_montgomery(out, x);
}

static void sc_fiat_to_montgomery(uint64_t out[4], const uint64_t x[4]) {
    fiat_25519_scalar_to_montgomery(out, x);
}

static void sc_fiat_to_bytes(uint8_t out[32], const uint64_t x[4]) {
    fiat_25519_scalar_to_bytes(out, x);
}

static void sc_fiat_from_bytes(uint64_t out[4], const uint8_t x[32]) {
    fiat_25519_scalar_from_bytes(out, x);
}

#else

#include "fiat_25519_scalar_32.h"

static void sc_split(uint32_t out[8], const uint64_t x[4]) {
    for (int i = 0; i < 4; i++) {
        out[2 * i] = (uint32_t)x[i];
        out[2 * i + 1] = (uint32_t)(x[i] >> 32);
    }
}

static void sc_join(uint64_t out[4], const uint32_t x[8]) {
    for (int i = 0; i < 4; i++)
        out[i] = (uint64_t)x[2 * i] | (uint64_t)x[2 * i + 1] << 32;
}

static void sc_fiat_mul(uint64_t out[4], const uint64_t x[4], const uint64_t y[4]) {
    uint32_t a[8], b[8], r[8];
    sc_split(a, x);
    sc_split(b, y);
    fiat_25519_scalar_mul(r, a, b);
    sc_join(out, r);
}

static void sc_fiat_add(uint64_t out[4], const uint64_t x[4], const uint64_t y[4]) {
    uint32_t a[8], b[8], r[8];
    sc_split(a, x);
    sc_split(b, y);
    fiat_25519_scalar_add(r, a, b);
    sc_join(out, r);
}

static void sc_fiat_sub(uint64_t out[4], const uint64_t x[4], const uint64_t y[4]) {
    uint32_t a[8], b[8], r[8];
    sc_split(a, x);
    sc_split(b, y);
    fiat_25519_scalar_sub(r, a, b);
    sc_join(out, r);
}

static void sc_fiat_opp(uint64_t out[4], const uint64_t x[4]) {
    uint32_t a[8], r[8];
    sc_split(a, x);
    fiat_25519_scalar_opp(r, a);
    sc_join(out, r);
}

static uint64_t sc_fiat_nonzero(const uint64_t x[4]) {
    uint32_t a[8], out;
    sc_split(a, x);
    fiat_25519_scalar_nonzero(&out, a);
    return out;
}

static void sc_fiat_from_montgomery(uint64_t out[4], const uint64_t x[4]) {
    uint32_t a[8], r[8];
    sc_split(a, x);
    fiat_25519_scalar_from_montgomery(r, a);
    sc_join(out, r);
}

static void sc_fiat_to_montgomery(uint64_t out[4], const uint64_t x[4]) {
    uint32_t a[8], r[8];
    sc_split(a, x);
    fiat_25519_scalar_to_montgomery(r, a);
    sc_join(out, r);
}

static void sc_fiat_to_bytes(uint8_t out[32], const uint64_t x[4]) {
    uint32_t a[8];
    sc_split(a, x);
    fiat_25519_scalar_to_bytes(out, a);
}

static void sc_fiat_from_bytes(uint64_t out[4], const uint8_t x[32]) {
    uint32_t r[8];
    fiat_25519_scalar_from_bytes(r, x);
    sc_join(out, r);
}

#endif

static uint64_t sc_le64(const uint8_t *p) {
    return (uint64_t)p[0] | (uint64_t)p[1] << 8 | (uint64_t)p[2] << 16 |
           (uint64_t)p[3] << 24 | (uint64_t)p[4] << 32 | (uint64_t)p[5] << 40 |
           (uint64_t)p[6] << 48 | (uint64_t)p[7] << 56;
}

/* 2^168 and 2^336 modulo l, in the Montgomery domain. */
static const Edwards25519Scalar sc_two168 = {
    {0x5b8ab432eac74798, 0x38afddd6de59d5d7, 0xa2c131b399411b7c, 0x6329a7ed9ce5a30}};
static const Edwards25519Scalar sc_two336 = {
    {0xbd3d108e2b35ecc5, 0x5c3a3718bdf9c90b, 0x63aa97a331b4f2ee, 0x3d217f5be65cb5c}};

/* l - 1, little endian. */
static const uint8_t sc_minus_one_bytes[32] = {
    236, 211, 245, 92, 26, 99, 18, 88, 214, 156, 247, 162, 222, 249, 222, 20,
    0,   0,   0,   0,  0,  0,  0,  0,  0,   0,   0,   0,   0,   0,   0,   16};

Edwards25519Scalar *burrow__sc_set(Edwards25519Scalar *s, const Edwards25519Scalar *x) {
    *s = *x;
    return s;
}

Edwards25519Scalar *burrow__sc_add(Edwards25519Scalar *s, const Edwards25519Scalar *x,
                                   const Edwards25519Scalar *y) {
    /* s = 1 * x + y mod l */
    sc_fiat_add(s->s, x->s, y->s);
    return s;
}

Edwards25519Scalar *burrow__sc_subtract(Edwards25519Scalar *s,
                                        const Edwards25519Scalar *x,
                                        const Edwards25519Scalar *y) {
    /* s = -1 * y + x mod l */
    sc_fiat_sub(s->s, x->s, y->s);
    return s;
}

Edwards25519Scalar *burrow__sc_negate(Edwards25519Scalar *s,
                                      const Edwards25519Scalar *x) {
    /* s = -1 * x + 0 mod l */
    sc_fiat_opp(s->s, x->s);
    return s;
}

Edwards25519Scalar *burrow__sc_multiply(Edwards25519Scalar *s,
                                        const Edwards25519Scalar *x,
                                        const Edwards25519Scalar *y) {
    /* s = x * y + 0 mod l */
    sc_fiat_mul(s->s, x->s, y->s);
    return s;
}

Edwards25519Scalar *burrow__sc_multiply_add(Edwards25519Scalar *s,
                                            const Edwards25519Scalar *x,
                                            const Edwards25519Scalar *y,
                                            const Edwards25519Scalar *z) {
    /* A copy of z in case it is s. */
    Edwards25519Scalar z_copy = *z;
    burrow__sc_multiply(s, x, y);
    return burrow__sc_add(s, s, &z_copy);
}

/* x mod l, for a little endian x shorter than 32 bytes. */
static Edwards25519Scalar *sc_set_short_bytes(Edwards25519Scalar *s, const uint8_t *x,
                                              Int n) {
    if (n >= 32)
        panic_str(BURROW_S(
            "edwards25519: internal error: setShortBytes called with a long string"));
    uint8_t buf[32] = {0};
    memcpy(buf, x, (size_t)n);
    sc_fiat_from_bytes(s->s, buf);
    sc_fiat_to_montgomery(s->s, s->s);
    return s;
}

Edwards25519Scalar *burrow__sc_set_uniform_bytes(Edwards25519Scalar *s, Slice x,
                                                 Error *err) {
    if (x.len != 64) {
        BURROW_OUT(err,
                   errors_new(error_allocator(),
                              BURROW_S("edwards25519: invalid SetUniformBytes input "
                                       "length")));
        return NULL;
    }
    const uint8_t *p = x.p;

    /* x is 512 bits, and fiat's from_bytes wants something below l, a little
     * over 252 bits. So x is read as a + b * 2^168 + c * 2^336 mod l, for three
     * shorter a, b and c, and 2^168 and 2^336 mod l are constants. */
    Edwards25519Scalar t;
    sc_set_short_bytes(s, p, 21);
    sc_set_short_bytes(&t, p + 21, 21);
    burrow__sc_add(s, s, burrow__sc_multiply(&t, &t, &sc_two168));
    sc_set_short_bytes(&t, p + 42, 22);
    burrow__sc_add(s, s, burrow__sc_multiply(&t, &t, &sc_two336));
    BURROW_OUT(err, (Error){0});
    return s;
}

bool burrow__sc_is_reduced(Slice s) {
    if (s.len != 32)
        return false;
    const uint8_t *p = s.p;

    uint64_t s0 = sc_le64(p), s1 = sc_le64(p + 8), s2 = sc_le64(p + 16),
             s3 = sc_le64(p + 24);
    uint64_t l0 = sc_le64(sc_minus_one_bytes), l1 = sc_le64(sc_minus_one_bytes + 8),
             l2 = sc_le64(sc_minus_one_bytes + 16),
             l3 = sc_le64(sc_minus_one_bytes + 24);

    /* l - 1 - s in constant time. A borrow out of the top means s > l - 1. */
    uint64_t b;
    (void)bits_sub64(l0, s0, 0, &b);
    (void)bits_sub64(l1, s1, b, &b);
    (void)bits_sub64(l2, s2, b, &b);
    (void)bits_sub64(l3, s3, b, &b);
    return b == 0;
}

Edwards25519Scalar *burrow__sc_set_canonical_bytes(Edwards25519Scalar *s, Slice x,
                                                   Error *err) {
    if (x.len != 32) {
        BURROW_OUT(err,
                   errors_new(error_allocator(), BURROW_S("invalid scalar length")));
        return NULL;
    }
    if (!burrow__sc_is_reduced(x)) {
        BURROW_OUT(err,
                   errors_new(error_allocator(), BURROW_S("invalid scalar encoding")));
        return NULL;
    }
    sc_fiat_from_bytes(s->s, x.p);
    sc_fiat_to_montgomery(s->s, s->s);
    BURROW_OUT(err, (Error){0});
    return s;
}

Edwards25519Scalar *burrow__sc_set_bytes_with_clamping(Edwards25519Scalar *s, Slice x,
                                                       Error *err) {
    /* The top bits of the clamping are lost to the reduction too, and do not
     * matter to edwards25519: they guard against a bug once seen in a generic
     * Montgomery ladder. */
    if (x.len != 32) {
        BURROW_OUT(err,
                   errors_new(error_allocator(),
                              BURROW_S("edwards25519: invalid SetBytesWithClamping "
                                       "input length")));
        return NULL;
    }

    /* The wide reduction from set_uniform_bytes, since clamping sets the 2^254
     * bit, which puts the value above the order. */
    uint8_t wide[64] = {0};
    memcpy(wide, x.p, 32);
    wide[0] &= 248;
    wide[31] &= 63;
    wide[31] |= 64;
    return burrow__sc_set_uniform_bytes(s, slice_from(wide, 64, 64, TYPE_BYTE), err);
}

void burrow__sc_bytes(const Edwards25519Scalar *s, uint8_t out[32]) {
    uint64_t ss[4];
    sc_fiat_from_montgomery(ss, s->s);
    sc_fiat_to_bytes(out, ss);
}

int burrow__sc_equal(const Edwards25519Scalar *s, const Edwards25519Scalar *t) {
    uint64_t diff[4];
    sc_fiat_sub(diff, s->s, t->s);
    uint64_t nonzero = sc_fiat_nonzero(diff);
    nonzero |= nonzero >> 32;
    nonzero |= nonzero >> 16;
    nonzero |= nonzero >> 8;
    nonzero |= nonzero >> 4;
    nonzero |= nonzero >> 2;
    nonzero |= nonzero >> 1;
    return (int)(~nonzero & 1);
}

void burrow__sc_non_adjacent_form(const Edwards25519Scalar *s, unsigned w,
                                  int8_t naf[256]) {
    /* Adapted, as Go's is, from curve25519-dalek, where it is documented:
     * https://github.com/dalek-cryptography/curve25519-dalek/blob/f630041af28e9a405255f98a8a93adca18e4315b/src/scalar.rs#L800-L871 */
    uint8_t b[32];
    burrow__sc_bytes(s, b);
    if (b[31] > 127)
        panic_str(BURROW_S("scalar has high bit set illegally"));
    if (w < 2)
        panic_str(BURROW_S("w must be at least 2 by the definition of NAF"));
    else if (w > 8)
        panic_str(BURROW_S("NAF digits must fit in int8"));

    memset(naf, 0, 256);
    uint64_t digits[5] = {0};
    for (Int i = 0; i < 4; i++)
        digits[i] = sc_le64(b + i * 8);

    uint64_t width = (uint64_t)1 << w;
    uint64_t window_mask = width - 1;

    unsigned pos = 0;
    uint64_t carry = 0;
    while (pos < 256) {
        unsigned index_u64 = pos / 64;
        unsigned index_bit = pos % 64;
        uint64_t bit_buf;
        if (index_bit < 64 - w) {
            /* This window's bits are in one word. */
            bit_buf = digits[index_u64] >> index_bit;
        } else {
            /* The rest of this word and the start of the next. */
            bit_buf = (digits[index_u64] >> index_bit) |
                      (digits[1 + index_u64] << (64 - index_bit));
        }

        /* The carry goes into this window. */
        uint64_t window = carry + (bit_buf & window_mask);

        if ((window & 1) == 0) {
            /* An even window keeps the carry. With carry 0 the next carry is 0,
             * and with carry 1, bit_buf & 1 was 1, so the next carry is 1. */
            pos += 1;
            continue;
        }

        if (window < width / 2) {
            carry = 0;
            naf[pos] = (int8_t)window;
        } else {
            carry = 1;
            naf[pos] = (int8_t)((int)window - (int)width);
        }

        pos += w;
    }
}

void burrow__sc_signed_radix16(const Edwards25519Scalar *s, int8_t digits[64]) {
    uint8_t b[32];
    burrow__sc_bytes(s, b);
    if (b[31] > 127)
        panic_str(BURROW_S("scalar has high bit set illegally"));

    /* The unsigned radix 16 digits. */
    for (Int i = 0; i < 32; i++) {
        digits[2 * i] = (int8_t)(b[i] & 15);
        digits[2 * i + 1] = (int8_t)((b[i] >> 4) & 15);
    }

    /* Recentred into -8 to 7. */
    for (int i = 0; i < 63; i++) {
        int8_t carry = (int8_t)((digits[i] + 8) >> 4);
        digits[i] = (int8_t)(digits[i] - (carry << 4));
        digits[i + 1] = (int8_t)(digits[i + 1] + carry);
    }
}
