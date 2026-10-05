/* crypto/internal/fips140/bigmod: constant time arithmetic modulo a number of
 * any size.
 *
 * Derived from Go's src/crypto/internal/fips140/bigmod/nat.go and
 * nat_noasm.go.
 * Go source: go1.27.1.
 *
 * Go's comments about //go:norace are about its race detector and are left
 * out, and its assembly for addMulVVW is the generic loop here, which the
 * sized versions call with a constant length so that the compiler can unroll
 * it. Go's Exp and the others make their scratch numbers with NewNat, which
 * the compiler puts on the stack while they fit in its preallocated words.
 * Here they are arrays on the stack of the same size, and the heap when the
 * modulus is bigger.
 *
 * Copyright 2021 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "bigmod.h"

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/math/bits.h"
#include "burrow/mem.h"
#include "burrow/mem/heap.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include <stdint.h>
#include <string.h>

#define W ((Uint)BIGMOD_W)
#define S ((Int)BIGMOD_S)
#define PRE BIGMOD_PREALLOC_LIMBS

static BigmodChoice not_(BigmodChoice c) {
    return 1 ^ c;
}

/* ctMask: all ones if on is 1, and all zeros if it is 0. */
static Uint ct_mask(BigmodChoice on) {
    return -(Uint)on;
}

/* ctEq: 1 if x == y, and 0 otherwise, in time that does not depend on them. */
static BigmodChoice ct_eq(Uint x, Uint y) {
    /* If x != y, then either x - y or y - x will generate a carry. */
    Uint c1, c2;
    (void)bits_sub(x, y, 0, &c1);
    (void)bits_sub(y, x, 0, &c2);
    return not_(c1 | c2);
}

/* x >> n for any n, with Go's meaning of 0 once n is the size of a word. */
static Uint shr(Uint x, Uint n) {
    return n >= W ? 0 : x >> n;
}

static void *bigmod_error(Error *err, const char *msg) {
    BURROW_OUT(err, errors_new(error_allocator(), str_from_cstr(msg)));
    return NULL;
}

static void out_of_memory(void) {
    panic_str(BURROW_S("bigmod: out of memory"));
}

/* ---------------------------------------------------------------- storage */

void bigmod_nat_init(BigmodNat *x, Alloc *a) {
    *x = (BigmodNat){NULL, 0, 0, a, false};
}

BigmodNat *bigmod_new_nat(Alloc *a) {
    BigmodNat *x =
        mem_alloc(a != NULL ? a : heap_allocator(), sizeof *x, _Alignof(BigmodNat));
    if (x == NULL)
        out_of_memory();
    bigmod_nat_init(x, a);
    return x;
}

void bigmod_nat_init_buf(BigmodNat *x, Uint *buf, Int cap) {
    *x = (BigmodNat){buf, 0, cap, NULL, true};
}

static Alloc *nat_alloc(const BigmodNat *x) {
    return x->a != NULL ? x->a : heap_allocator();
}

void bigmod_nat_free(BigmodNat *x) {
    if (x->limbs != NULL && !x->borrowed)
        mem_free(nat_alloc(x), x->limbs, (size_t)x->cap * sizeof(Uint), _Alignof(Uint));
    x->limbs = NULL;
    x->len = 0;
    x->cap = 0;
    x->borrowed = false;
}

/* make([]uint, n) as x's words, keeping the first keep of the old ones. */
static void nat_grow(BigmodNat *x, Int n, Int keep) {
    Uint *p = mem_alloc(nat_alloc(x), (size_t)n * sizeof(Uint), _Alignof(Uint));
    if (p == NULL)
        out_of_memory();
    memset(p, 0, (size_t)n * sizeof(Uint));
    if (keep > 0)
        memcpy(p, x->limbs, (size_t)keep * sizeof(Uint));
    Int len = x->len;
    bigmod_nat_free(x);
    x->limbs = p;
    x->cap = n;
    x->len = len;
}

BigmodNat *bigmod_nat_expand(BigmodNat *x, Int n) {
    if (x->len > n)
        panic_str(BURROW_S("bigmod: internal error: shrinking nat"));
    if (x->cap < n) {
        nat_grow(x, n, x->len);
        x->len = n;
        return x;
    }
    memset(x->limbs + x->len, 0, (size_t)(n - x->len) * sizeof(Uint));
    x->len = n;
    return x;
}

BigmodNat *bigmod_nat_reset(BigmodNat *x, Int n) {
    if (x->cap < n) {
        nat_grow(x, n, 0);
        x->len = n;
        return x;
    }
    /* Clear both the returned limbs and the previously used ones. */
    Int clear = n > x->len ? n : x->len;
    if (clear > 0)
        memset(x->limbs, 0, (size_t)clear * sizeof(Uint));
    x->len = n;
    return x;
}

/* bigEndianUint: the S bytes at buf, big endian. */
static Uint big_endian_uint(const uint8_t *buf) {
    Uint v = 0;
    for (Int i = 0; i < S; i++)
        v = v << 8 | buf[i];
    return v;
}

/* setBytes: x = b, big endian, in the words x already has. */
static bool nat_set_bytes(BigmodNat *x, Slice b) {
    const uint8_t *p = b.p;
    Int i = b.len, k = 0;
    while (k < x->len && i >= S) {
        x->limbs[k] = big_endian_uint(p + i - S);
        i -= S;
        k++;
    }
    for (Uint s = 0; s < W && k < x->len && i > 0; s += 8) {
        x->limbs[k] |= (Uint)p[i - 1] << s;
        i--;
    }
    return i <= 0;
}

BigmodNat *bigmod_nat_reset_to_bytes(BigmodNat *x, Slice b) {
    bigmod_nat_reset(x, (b.len + S - 1) / S);
    if (!nat_set_bytes(x, b))
        panic_str(BURROW_S("bigmod: internal error: bad arithmetic"));
    return bigmod_nat_trim(x);
}

BigmodNat *bigmod_nat_trim(BigmodNat *x) {
    /* Trim most significant (trailing in little-endian) zero limbs. We assume
     * comparison with zero (but not the branch) is constant time. */
    for (Int i = x->len - 1; i >= 0; i--) {
        if (x->limbs[i] != 0)
            break;
        x->len = i;
    }
    return x;
}

BigmodNat *bigmod_nat_set(BigmodNat *x, const BigmodNat *y) {
    if (x == y)
        return x;
    bigmod_nat_reset(x, y->len);
    if (y->len > 0)
        memcpy(x->limbs, y->limbs, (size_t)y->len * sizeof(Uint));
    return x;
}

Slice bigmod_nat_bits(BigmodNat *x) {
    return slice_from(x->limbs, x->len, x->len, TYPE_UINT);
}

BigmodNat *bigmod_nat_set_bits(BigmodNat *x, Slice y) {
    bigmod_nat_reset(x, y.len);
    if (y.len > 0)
        memcpy(x->limbs, y.p, (size_t)y.len * sizeof(Uint));
    return x;
}

Slice bigmod_nat_bytes(const BigmodNat *x, Alloc *a, const BigmodModulus *m) {
    Int i = bigmod_modulus_size(m);
    uint8_t *bytes = NULL;
    if (i > 0) {
        bytes = mem_alloc(a != NULL ? a : heap_allocator(), (size_t)i, 1);
        if (bytes == NULL)
            out_of_memory();
        memset(bytes, 0, (size_t)i);
    }
    Int size = i;
    for (Int k = 0; k < x->len; k++) {
        Uint limb = x->limbs[k];
        for (Int j = 0; j < S; j++) {
            i--;
            if (i < 0) {
                if (limb == 0)
                    break;
                panic_str(BURROW_S("bigmod: modulus is smaller than nat"));
            }
            bytes[i] = (uint8_t)limb;
            limb >>= 8;
        }
    }
    return slice_from(bytes, size, size, TYPE_BYTE);
}

/* cmpGeq: 1 if x >= y, and 0 otherwise, for x and y of the same length. */
static BigmodChoice nat_cmp_geq(const BigmodNat *x, const BigmodNat *y) {
    Uint c = 0;
    for (Int i = 0; i < x->len; i++)
        (void)bits_sub(x->limbs[i], y->limbs[i], c, &c);
    /* If there was a carry, then subtracting y underflowed, so x is not
     * greater than or equal to y. */
    return not_(c);
}

/* assign: x = y if on is 1, and nothing otherwise, for x and y of the same
 * length. */
static BigmodNat *nat_assign(BigmodNat *x, BigmodChoice on, const BigmodNat *y) {
    Uint mask = ct_mask(on);
    for (Int i = 0; i < x->len; i++)
        x->limbs[i] ^= mask & (x->limbs[i] ^ y->limbs[i]);
    return x;
}

/* add: x += y, returning the carry, for x and y of the same length. */
static Uint nat_add(BigmodNat *x, const BigmodNat *y) {
    Uint c = 0;
    for (Int i = 0; i < x->len; i++)
        x->limbs[i] = bits_add(x->limbs[i], y->limbs[i], c, &c);
    return c;
}

/* sub: x -= y, returning the borrow, for x and y of the same length. */
static Uint nat_sub(BigmodNat *x, const BigmodNat *y) {
    Uint c = 0;
    for (Int i = 0; i < x->len; i++)
        x->limbs[i] = bits_sub(x->limbs[i], y->limbs[i], c, &c);
    return c;
}

BigmodNat *bigmod_nat_set_bytes(BigmodNat *x, Slice b, const BigmodModulus *m,
                                Error *err) {
    bigmod_nat_reset_for(x, m);
    if (!nat_set_bytes(x, b))
        return bigmod_error(err, "input overflows the modulus size");
    if (nat_cmp_geq(x, &m->nat) == 1)
        return bigmod_error(err, "input overflows the modulus");
    return x;
}

/* bitLen: bits.Len, leaking only the length of n and not its value. */
static Int bit_len(Uint n) {
    Int len = 0;
    /* We assume, here and elsewhere, that comparison to zero is constant time
     * with respect to different non-zero values. */
    while (n != 0) {
        len++;
        n >>= 1;
    }
    return len;
}

BigmodNat *bigmod_nat_set_overflowing_bytes(BigmodNat *x, Slice b,
                                            const BigmodModulus *m, Error *err) {
    bigmod_nat_reset_for(x, m);
    if (!nat_set_bytes(x, b))
        return bigmod_error(err, "input overflows the modulus size");
    /* setBytes would have returned an error if the input overflowed the limb
     * size of the modulus, so now we only need to check if the most
     * significant limb of x has more bits than the most significant limb of
     * the modulus. */
    if (bit_len(x->limbs[x->len - 1]) > bit_len(m->nat.limbs[m->nat.len - 1]))
        return bigmod_error(err, "input overflows the modulus size");
    bigmod_nat_maybe_subtract_modulus(x, 0, m);
    return x;
}

BigmodNat *bigmod_nat_set_uint(BigmodNat *x, Uint y) {
    bigmod_nat_reset(x, 1);
    x->limbs[0] = y;
    return x;
}

BigmodChoice bigmod_nat_equal(const BigmodNat *x, const BigmodNat *y) {
    BigmodChoice equal = 1;
    for (Int i = 0; i < x->len; i++)
        equal &= ct_eq(x->limbs[i], y->limbs[i]);
    return equal;
}

BigmodChoice bigmod_nat_is_zero(const BigmodNat *x) {
    BigmodChoice zero = 1;
    for (Int i = 0; i < x->len; i++)
        zero &= ct_eq(x->limbs[i], 0);
    return zero;
}

BigmodChoice bigmod_nat_is_one(const BigmodNat *x) {
    if (x->len == 0)
        return 0;
    BigmodChoice one = ct_eq(x->limbs[0], 1);
    for (Int i = 1; i < x->len; i++)
        one &= ct_eq(x->limbs[i], 0);
    return one;
}

BigmodChoice bigmod_nat_is_minus_one(const BigmodNat *x, const BigmodModulus *m) {
    Uint buf[PRE];
    BigmodNat minus_one;
    bigmod_nat_init_buf(&minus_one, buf, PRE);
    bigmod_nat_set(&minus_one, &m->nat);
    bigmod_nat_sub_one(&minus_one, m);
    BigmodChoice r = bigmod_nat_equal(x, &minus_one);
    bigmod_nat_free(&minus_one);
    return r;
}

BigmodChoice bigmod_nat_is_odd(const BigmodNat *x) {
    if (x->len == 0)
        return 0;
    return x->limbs[0] & 1;
}

Uint bigmod_nat_trailing_zero_bits_var_time(const BigmodNat *x) {
    Uint t = 0;
    for (Int i = 0; i < x->len; i++) {
        Uint l = x->limbs[i];
        if (l == 0) {
            t += W;
            continue;
        }
        t += (Uint)bits_trailing_zeros(l);
        break;
    }
    return t;
}

BigmodNat *bigmod_nat_shift_right_var_time(BigmodNat *x, Uint n) {
    Int size = x->len;
    Uint shift = n % W;
    Uint shift_limbs = n / W;

    Uint *shifted = NULL;
    Int shifted_len = 0;
    if (shift_limbs < (Uint)size) {
        shifted = x->limbs + shift_limbs;
        shifted_len = size - (Int)shift_limbs;
    }

    for (Int i = 0; i < size; i++) {
        if (i >= shifted_len) {
            x->limbs[i] = 0;
            continue;
        }
        x->limbs[i] = shifted[i] >> shift;
        if (i + 1 < shifted_len && shift != 0)
            x->limbs[i] |= shifted[i + 1] << (W - shift);
    }
    return x;
}

Int bigmod_nat_bit_len_var_time(const BigmodNat *x) {
    for (Int i = x->len - 1; i >= 0; i--) {
        if (x->limbs[i] != 0)
            return i * (Int)W + bit_len(x->limbs[i]);
    }
    return 0;
}

/* ---------------------------------------------------------------- Modulus */

BigmodNat *bigmod_nat_expand_for(BigmodNat *x, const BigmodModulus *m) {
    return bigmod_nat_expand(x, m->nat.len);
}

BigmodNat *bigmod_nat_reset_for(BigmodNat *x, const BigmodModulus *m) {
    return bigmod_nat_reset(x, m->nat.len);
}

/* rr: R*R with R = 2^(_W * n) and n = len(m.nat.limbs), into m->rr. */
static void modulus_rr(BigmodModulus *m) {
    BigmodNat *rr = &m->rr;
    bigmod_nat_expand_for(rr, m);
    Uint n = (Uint)rr->len;
    Uint m_len = (Uint)bigmod_modulus_bit_len(m);
    Uint log_r = W * n;

    /* We start by computing R = 2^(_W * n) mod m. We can get pretty close, to
     * 2^⌊log₂m⌋, by setting the highest bit we can without having to
     * reduce. */
    rr->limbs[n - 1] = (Uint)1 << ((m_len - 1) % W);
    /* Then we double until we reach 2^(_W * n). */
    for (Uint i = m_len - 1; i < log_r; i++)
        bigmod_nat_add(rr, rr, m);

    /* Next we need to get from R to 2^(_W * n) R mod m (aka from one to R in
     * the Montgomery domain, meaning we can use Montgomery multiplication
     * now). We could do that by doubling _W * n times, or with a
     * square-and-double chain log2(_W * n) long. Turns out the fastest thing
     * is to start out with doublings, and switch to square-and-double once the
     * exponent is large enough to justify the cost of the multiplications. */

    /* The threshold is selected experimentally as a linear function of n. */
    Uint threshold = n / 4;

    /* We calculate how many of the most-significant bits of the exponent we
     * can compute before crossing the threshold, and we do it with
     * doublings. */
    Uint i = W;
    while (shr(log_r, i) <= threshold)
        i--;
    for (Uint k = 0; k < shr(log_r, i); k++)
        bigmod_nat_add(rr, rr, m);

    /* Then we process the remaining bits of the exponent with a
     * square-and-double chain. */
    while (i > 0) {
        bigmod_nat_montgomery_mul(rr, rr, rr, m);
        i--;
        if (((log_r >> i) & 1) != 0)
            bigmod_nat_add(rr, rr, m);
    }
}

/* minusInverseModW: -x⁻¹ mod 2^_W, for x odd. */
static Uint minus_inverse_mod_w(Uint x) {
    /* Every iteration of this loop doubles the least-significant bits of
     * correct inverse in y. The first three bits are already correct (1⁻¹ =
     * 1, 3⁻¹ = 3, 5⁻¹ = 5, and 7⁻¹ = 7 mod 8), so doubling five times is
     * enough for 64 bits (and wastes only one iteration for 32 bits).
     *
     * See https://crypto.stackexchange.com/a/47496. */
    Uint y = x;
    for (int i = 0; i < 5; i++)
        y = y * (2 - x * y);
    return -y;
}

/* newModulus: the struct around n, which it takes, from a. */
static BigmodModulus *new_modulus(Alloc *a, BigmodNat *n, Error *err) {
    if (bigmod_nat_is_zero(n) == 1 || bigmod_nat_is_one(n) == 1) {
        bigmod_nat_free(n);
        return bigmod_error(err, "modulus must be > 1");
    }
    BigmodModulus *m =
        mem_alloc(a != NULL ? a : heap_allocator(), sizeof *m, _Alignof(BigmodModulus));
    if (m == NULL) {
        bigmod_nat_free(n);
        out_of_memory();
    }
    m->nat = *n;
    m->odd = false;
    m->m0inv = 0;
    bigmod_nat_init(&m->rr, a);
    if (bigmod_nat_is_odd(&m->nat) == 1) {
        m->odd = true;
        m->m0inv = minus_inverse_mod_w(m->nat.limbs[0]);
        modulus_rr(m);
    }
    return m;
}

BigmodModulus *bigmod_new_modulus(Alloc *a, Slice b, Error *err) {
    BigmodNat n;
    bigmod_nat_init(&n, a);
    bigmod_nat_reset_to_bytes(&n, b);
    return new_modulus(a, &n, err);
}

BigmodModulus *bigmod_new_modulus_product(Alloc *a, Slice x_bytes, Slice y_bytes,
                                          Error *err) {
    BigmodNat x, y, n;
    bigmod_nat_init(&x, NULL);
    bigmod_nat_init(&y, NULL);
    bigmod_nat_init(&n, a);
    bigmod_nat_reset_to_bytes(&x, x_bytes);
    bigmod_nat_reset_to_bytes(&y, y_bytes);
    bigmod_nat_reset(&n, x.len + y.len);
    for (Int i = 0; i < y.len; i++)
        n.limbs[i + x.len] =
            bigmod_add_mul_vvw(n.limbs + i, x.limbs, y.limbs[i], x.len);
    bigmod_nat_free(&x);
    bigmod_nat_free(&y);
    return new_modulus(a, bigmod_nat_trim(&n), err);
}

void bigmod_modulus_free(BigmodModulus *m) {
    Alloc *a = nat_alloc(&m->nat);
    bigmod_nat_free(&m->nat);
    bigmod_nat_free(&m->rr);
    mem_free(a, m, sizeof *m, _Alignof(BigmodModulus));
}

Int bigmod_modulus_size(const BigmodModulus *m) {
    return (bigmod_modulus_bit_len(m) + 7) / 8;
}

Int bigmod_modulus_bit_len(const BigmodModulus *m) {
    return bigmod_nat_bit_len_var_time(&m->nat);
}

BigmodNat *bigmod_modulus_nat(const BigmodModulus *m, Alloc *a) {
    /* Make a copy so that the caller can't modify m.nat or alias it with
     * another Nat in a modulus operation. */
    BigmodNat *n = bigmod_new_nat(a);
    return bigmod_nat_set(n, &m->nat);
}

/* ------------------------------------------------------------- arithmetic */

BigmodNat *bigmod_nat_shift_in(BigmodNat *x, Uint y, const BigmodModulus *m) {
    Uint buf[PRE];
    BigmodNat d;
    bigmod_nat_init_buf(&d, buf, PRE);
    bigmod_nat_reset_for(&d, m);

    Int size = m->nat.len;
    Uint *x_limbs = x->limbs, *d_limbs = d.limbs;
    const Uint *m_limbs = m->nat.limbs;

    /* Each iteration of this loop computes x = 2x + b mod m, where b is a bit
     * from y. Effectively, it left-shifts x and adds y one bit at a time,
     * reducing it every time.
     *
     * To do the reduction, each iteration computes both 2x + b and 2x + b -
     * m. The next iteration (and finally the return line) will use either
     * result based on whether 2x + b overflows m. */
    BigmodChoice need_subtraction = 0;
    for (int i = (int)W - 1; i >= 0; i--) {
        Uint carry = (y >> i) & 1;
        Uint borrow = 0;
        Uint mask = ct_mask(need_subtraction);
        for (Int j = 0; j < size; j++) {
            Uint l = x_limbs[j] ^ (mask & (x_limbs[j] ^ d_limbs[j]));
            x_limbs[j] = bits_add(l, l, carry, &carry);
            d_limbs[j] = bits_sub(x_limbs[j], m_limbs[j], borrow, &borrow);
        }
        /* Like in maybeSubtractModulus, we need the subtraction if either it
         * didn't underflow (meaning 2x + b > m) or if computing 2x + b
         * overflowed (meaning 2x + b > 2^_W*n > m). */
        need_subtraction = not_(borrow) | carry;
    }
    nat_assign(x, need_subtraction, &d);
    bigmod_nat_free(&d);
    return x;
}

BigmodNat *bigmod_nat_mod(BigmodNat *out, const BigmodNat *x, const BigmodModulus *m) {
    bigmod_nat_reset_for(out, m);
    /* Working our way from the most significant to the least significant
     * limb, we can insert each limb at the least significant position,
     * shifting all previous limbs left by _W. This way each limb will get
     * shifted by the correct number of bits. We can insert at least N - 1
     * limbs without overflowing m. After that, we need to reduce every time
     * we shift. */
    Int i = x->len - 1;
    /* For the first N - 1 limbs we can skip the actual shifting and position
     * them at the shifted position, which starts at min(N - 2, i). */
    Int start = m->nat.len - 2;
    if (i < start)
        start = i;
    for (Int j = start; j >= 0; j--) {
        out->limbs[j] = x->limbs[i];
        i--;
    }
    /* We shift in the remaining limbs, reducing modulo m each time. */
    while (i >= 0) {
        bigmod_nat_shift_in(out, x->limbs[i], m);
        i--;
    }
    return out;
}

void bigmod_nat_maybe_subtract_modulus(BigmodNat *x, BigmodChoice always,
                                       const BigmodModulus *m) {
    Uint buf[PRE];
    BigmodNat t;
    bigmod_nat_init_buf(&t, buf, PRE);
    bigmod_nat_set(&t, x);
    Uint underflow = nat_sub(&t, &m->nat);
    /* We keep the result if x - m didn't underflow (meaning x >= m) or if
     * always was set. */
    BigmodChoice keep = not_(underflow) | always;
    nat_assign(x, keep, &t);
    bigmod_nat_free(&t);
}

BigmodNat *bigmod_nat_sub(BigmodNat *x, const BigmodNat *y, const BigmodModulus *m) {
    Uint underflow = nat_sub(x, y);
    /* If the subtraction underflowed, add m. */
    Uint buf[PRE];
    BigmodNat t;
    bigmod_nat_init_buf(&t, buf, PRE);
    bigmod_nat_set(&t, x);
    (void)nat_add(&t, &m->nat);
    nat_assign(x, underflow, &t);
    bigmod_nat_free(&t);
    return x;
}

BigmodNat *bigmod_nat_sub_one(BigmodNat *x, const BigmodModulus *m) {
    Uint buf[PRE];
    BigmodNat one;
    bigmod_nat_init_buf(&one, buf, PRE);
    bigmod_nat_expand_for(&one, m);
    one.limbs[0] = 1;
    /* Sub asks for x to be reduced modulo m, while SubOne doesn't, but when y
     * = 1, it works, and this is an internal use. */
    bigmod_nat_sub(x, &one, m);
    bigmod_nat_free(&one);
    return x;
}

BigmodNat *bigmod_nat_add(BigmodNat *x, const BigmodNat *y, const BigmodModulus *m) {
    Uint overflow = nat_add(x, y);
    bigmod_nat_maybe_subtract_modulus(x, overflow, m);
    return x;
}

BigmodNat *bigmod_nat_montgomery_representation(BigmodNat *x, const BigmodModulus *m) {
    /* A Montgomery multiplication (which computes a * b / R) by R * R works
     * out to a multiplication by R, which takes the value out of the
     * Montgomery domain. */
    return bigmod_nat_montgomery_mul(x, x, &m->rr, m);
}

BigmodNat *bigmod_nat_montgomery_reduction(BigmodNat *x, const BigmodModulus *m) {
    /* By Montgomery multiplying with 1 not in Montgomery representation, we
     * convert out back from Montgomery representation, because it works out
     * to dividing by R. */
    Uint buf[PRE];
    BigmodNat one;
    bigmod_nat_init_buf(&one, buf, PRE);
    bigmod_nat_expand_for(&one, m);
    one.limbs[0] = 1;
    bigmod_nat_montgomery_mul(x, x, &one, m);
    bigmod_nat_free(&one);
    return x;
}

/* T: the 2n words of scratch montgomeryMul and Mul need, from buf while they
 * fit and from the heap when not. */
static Uint *scratch_words(Uint *buf, Int cap, Int n) {
    if (n <= cap) {
        memset(buf, 0, (size_t)n * sizeof(Uint));
        return buf;
    }
    Uint *p = mem_alloc(heap_allocator(), (size_t)n * sizeof(Uint), _Alignof(Uint));
    if (p == NULL)
        out_of_memory();
    memset(p, 0, (size_t)n * sizeof(Uint));
    return p;
}

static void scratch_free(Uint *p, Uint *buf, Int n) {
    if (p != buf)
        mem_free(heap_allocator(), p, (size_t)n * sizeof(Uint), _Alignof(Uint));
}

BigmodNat *bigmod_nat_montgomery_mul(BigmodNat *x, const BigmodNat *a,
                                     const BigmodNat *b, const BigmodModulus *m) {
    Int n = m->nat.len;
    const Uint *m_limbs = m->nat.limbs;
    const Uint *a_limbs = a->limbs;
    const Uint *b_limbs = b->limbs;

    Uint buf[PRE * 2];
    Uint *t = scratch_words(buf, PRE * 2, n * 2);

    /* This loop implements Word-by-Word Montgomery Multiplication, as
     * described in Algorithm 4 (Fig. 3) of "Efficient Software
     * Implementations of Modular Exponentiation" by Shay Gueron
     * [https://eprint.iacr.org/2011/239.pdf].
     *
     * Step 1 (T = a × b) is computed as a large pen-and-paper column
     * multiplication of two numbers with n base-2^_W digits. Instead of
     * running two loops, one for Step 1 and one for Steps 2–6, the result of
     * Step 1 is computed during the next loop. This is possible because each
     * iteration only uses T[i] in Step 2 and then discards it in Step 6. */
    Uint c = 0;
    for (Int i = 0; i < n; i++) {
        Uint d = b_limbs[i];
        Uint c1 = bigmod_add_mul_vvw(t + i, a_limbs, d, n);

        /* Step 6 is replaced by shifting the virtual window we operate over:
         * T of the algorithm is T[i:] for us. That means that T1 in Step 2 (T
         * mod 2^_W) is simply T[i]. k0 in Step 3 is our m0inv. */
        Uint y = t[i] * m->m0inv;

        /* Step 4 and 5 add Y × m to T, which as mentioned above is stored at
         * T[i:]. The two carries (from a × d and Y × m) are added up in the
         * next word T[n+i], and the carry bit from that addition is brought
         * forward to the next iteration. */
        Uint c2 = bigmod_add_mul_vvw(t + i, m_limbs, y, n);
        t[n + i] = bits_add(c1, c2, c, &c);
    }

    /* Finally for Step 7 we copy the final T window into x, and subtract m if
     * necessary (which as explained in maybeSubtractModulus can be the case
     * both if x >= m, or if x overflowed).
     *
     * The paper suggests in Section 4 that we can do an "Almost Montgomery
     * Multiplication" by subtracting only in the overflow case, but the cost
     * is very similar since the constant time subtraction tells us if x >= m
     * as a side effect, and taking care of the broken invariant is highly
     * undesirable (see https://go.dev/issue/13907). */
    bigmod_nat_reset(x, n);
    memcpy(x->limbs, t + n, (size_t)n * sizeof(Uint));
    scratch_free(t, buf, n * 2);
    bigmod_nat_maybe_subtract_modulus(x, c, m);
    return x;
}

Uint bigmod_add_mul_vvw(Uint *z, const Uint *x, Uint y, Int len) {
    Uint carry = 0;
    for (Int i = 0; i < len; i++) {
        Uint lo;
        Uint hi = bits_mul(x[i], y, &lo);
        Uint c;
        lo = bits_add(lo, z[i], 0, &c);
        /* We use bits.Add with zero to get an add-with-carry instruction that
         * absorbs the carry from the previous bits.Add. */
        hi = bits_add(hi, 0, c, &c);
        lo = bits_add(lo, carry, 0, &c);
        hi = bits_add(hi, 0, c, &c);
        carry = hi;
        z[i] = lo;
    }
    return carry;
}

Uint bigmod_add_mul_vvw1024(Uint *z, const Uint *x, Uint y) {
    return bigmod_add_mul_vvw(z, x, y, 1024 / BIGMOD_W);
}

Uint bigmod_add_mul_vvw1536(Uint *z, const Uint *x, Uint y) {
    return bigmod_add_mul_vvw(z, x, y, 1536 / BIGMOD_W);
}

Uint bigmod_add_mul_vvw2048(Uint *z, const Uint *x, Uint y) {
    return bigmod_add_mul_vvw(z, x, y, 2048 / BIGMOD_W);
}

BigmodNat *bigmod_nat_mul(BigmodNat *x, const BigmodNat *y, const BigmodModulus *m) {
    if (m->odd) {
        /* A Montgomery multiplication by a value out of the Montgomery domain
         * takes the result out of Montgomery representation. */
        Uint buf[PRE];
        BigmodNat xr;
        bigmod_nat_init_buf(&xr, buf, PRE);
        bigmod_nat_set(&xr, x);
        bigmod_nat_montgomery_representation(&xr, m); /* xR = x * R mod m */
        bigmod_nat_montgomery_mul(x, &xr, y, m);      /* x = xR * y / R mod m */
        bigmod_nat_free(&xr);
        return x;
    }

    Int n = m->nat.len;
    Uint buf[PRE * 2];
    Uint *t = scratch_words(buf, PRE * 2, n * 2);

    /* T = x * y */
    for (Int i = 0; i < n; i++)
        t[n + i] = bigmod_add_mul_vvw(t + i, x->limbs, y->limbs[i], n);

    /* x = T mod m */
    BigmodNat tn;
    bigmod_nat_init_buf(&tn, t, n * 2);
    tn.len = n * 2;
    bigmod_nat_mod(x, &tn, m);
    scratch_free(t, buf, n * 2);
    return x;
}

BigmodNat *bigmod_nat_exp(BigmodNat *out, const BigmodNat *x, Slice e,
                          const BigmodModulus *m) {
    if (!m->odd)
        panic_str(BURROW_S("bigmod: modulus for Exp must be odd"));

    /* We use a 4 bit window. For our RSA workload, 4 bit windows are faster
     * than 2 bit windows, but use an extra 12 nats worth of scratch space.
     * Using bit sizes that don't divide 8 are more complex to implement, but
     * are likely to be more efficient if necessary. */

    enum { TABLE = (1 << 4) - 1 };
    Uint bufs[TABLE + 1][PRE];
    BigmodNat table[TABLE]; /* table[i] = x ^ (i+1) */
    for (int i = 0; i < TABLE; i++)
        bigmod_nat_init_buf(&table[i], bufs[i], PRE);
    bigmod_nat_set(&table[0], x);
    bigmod_nat_montgomery_representation(&table[0], m);
    for (int i = 1; i < TABLE; i++)
        bigmod_nat_montgomery_mul(&table[i], &table[i - 1], &table[0], m);

    bigmod_nat_reset_for(out, m);
    out->limbs[0] = 1;
    bigmod_nat_montgomery_representation(out, m);
    BigmodNat tmp;
    bigmod_nat_init_buf(&tmp, bufs[TABLE], PRE);
    bigmod_nat_expand_for(&tmp, m);
    const uint8_t *eb = e.p;
    for (Int bi = 0; bi < e.len; bi++) {
        uint8_t b = eb[bi];
        const int js[2] = {4, 0};
        for (int ji = 0; ji < 2; ji++) {
            /* Square four times. Optimization note: this can be implemented
             * more efficiently than with generic Montgomery multiplication. */
            bigmod_nat_montgomery_mul(out, out, out, m);
            bigmod_nat_montgomery_mul(out, out, out, m);
            bigmod_nat_montgomery_mul(out, out, out, m);
            bigmod_nat_montgomery_mul(out, out, out, m);

            /* Select x^k in constant time from the table. */
            Uint k = (Uint)((b >> js[ji]) & 0xf);
            for (int i = 0; i < TABLE; i++)
                nat_assign(&tmp, ct_eq(k, (Uint)(i + 1)), &table[i]);

            /* Multiply by x^k, discarding the result if k = 0. */
            bigmod_nat_montgomery_mul(&tmp, out, &tmp, m);
            nat_assign(out, not_(ct_eq(k, 0)), &tmp);
        }
    }
    for (int i = 0; i < TABLE; i++)
        bigmod_nat_free(&table[i]);
    bigmod_nat_free(&tmp);

    return bigmod_nat_montgomery_reduction(out, m);
}

BigmodNat *bigmod_nat_exp_short_var_time(BigmodNat *out, const BigmodNat *x, Uint e,
                                         const BigmodModulus *m) {
    if (!m->odd)
        panic_str(BURROW_S("bigmod: modulus for ExpShortVarTime must be odd"));
    /* For short exponents, precomputing a table and using a window like in
     * Exp doesn't pay off. Instead, we do a simple conditional
     * square-and-multiply chain, skipping the initial run of zeroes. */
    Uint buf[PRE];
    BigmodNat xr;
    bigmod_nat_init_buf(&xr, buf, PRE);
    bigmod_nat_set(&xr, x);
    bigmod_nat_montgomery_representation(&xr, m);
    bigmod_nat_set(out, &xr);
    for (Uint i = W - (Uint)bits_len(e) + 1; i < W; i++) {
        bigmod_nat_montgomery_mul(out, out, out, m);
        if (((e >> (W - i - 1)) & 1) != 0)
            bigmod_nat_montgomery_mul(out, out, &xr, m);
    }
    bigmod_nat_free(&xr);
    return bigmod_nat_montgomery_reduction(out, m);
}

static void rshift1(BigmodNat *a, Uint carry) {
    Int size = a->len;
    for (Int i = 0; i < size; i++) {
        a->limbs[i] >>= 1;
        if (i + 1 < size)
            a->limbs[i] |= a->limbs[i + 1] << (W - 1);
        else
            a->limbs[i] |= carry << (W - 1);
    }
}

/* &Modulus{nat: n}: a modulus that is only its number, which is all Add needs
 * of one. */
static BigmodModulus modulus_view(const BigmodNat *n) {
    BigmodModulus m;
    memset(&m, 0, sizeof m);
    m.nat = *n;
    m.nat.borrowed = true;
    return m;
}

/* extendedGCD: u and A such that u = GCD(a, m) = A*a - B*m, into u and A,
 * which the caller frees. u has the size of the larger of a and m, and A the
 * size of m. NULL, or the error's text when either a or m is zero, or they
 * are both even. */
static const char *extended_gcd(const BigmodNat *a, const BigmodNat *m, BigmodNat *u,
                                BigmodNat *A) {
    /* This is the extended binary GCD algorithm described in the Handbook of
     * Applied Cryptography, Algorithm 14.61, adapted by BoringSSL to bound
     * coefficients and avoid negative numbers. For more details and proof of
     * correctness, see https://github.com/mit-plv/fiat-crypto/pull/333/files.
     *
     * Following the proof linked in the PR above, the changes are:
     *
     * 1. Negate [B] and [C] so they are positive. The invariant now involves
     *    a subtraction.
     * 2. If step 2 (both [x] and [y] are even) runs, abort immediately. This
     *    case needs to be handled by the caller.
     * 3. Subtract copies of [x] and [y] as needed in step 6 (both [u] and [v]
     *    are odd) so coefficients stay in bounds.
     * 4. Replace the [u >= v] check with [u > v]. This changes the end
     *    condition to [v = 0] rather than [u = 0]. This saves an extra
     *    subtraction due to which coefficients were negated.
     * 5. Rename x and y to a and n, to capture that one is a modulus.
     * 6. Rearrange steps 4 through 6 slightly. Merge the loops in steps 4 and
     *    5 into the main loop (step 7's goto), and move step 6 to the start of
     *    the loop iteration, ensuring each loop iteration halves at least one
     *    value.
     *
     * Note this algorithm does not handle either input being zero. */

    if (bigmod_nat_is_zero(a) == 1 || bigmod_nat_is_zero(m) == 1)
        return "extendedGCD: a or m is zero";
    if (bigmod_nat_is_odd(a) == 0 && bigmod_nat_is_odd(m) == 0)
        return "extendedGCD: both a and m are even";

    Int size = a->len > m->len ? a->len : m->len;
    bigmod_nat_expand(bigmod_nat_set(u, a), size);
    BigmodNat v, B, C, D;
    bigmod_nat_init(&v, NULL);
    bigmod_nat_init(&B, NULL);
    bigmod_nat_init(&C, NULL);
    bigmod_nat_init(&D, NULL);
    bigmod_nat_expand(bigmod_nat_set(&v, m), size);

    bigmod_nat_reset(A, m->len);
    A->limbs[0] = 1;
    bigmod_nat_reset(&B, a->len);
    bigmod_nat_reset(&C, m->len);
    bigmod_nat_reset(&D, a->len);
    D.limbs[0] = 1;

    BigmodModulus mod_m = modulus_view(m), mod_a = modulus_view(a);

    /* Before and after each loop iteration, the following hold:
     *
     *   u = A*a - B*m
     *   v = D*m - C*a
     *   0 < u <= a
     *   0 <= v <= m
     *   0 <= A < m
     *   0 <= B <= a
     *   0 <= C < m
     *   0 <= D <= a
     *
     * After each loop iteration, u and v only get smaller, and at least one
     * of them shrinks by at least a factor of two. */
    for (;;) {
        /* If both u and v are odd, subtract the smaller from the larger. If u
         * = v, we need to subtract from v to hit the modified exit
         * condition. */
        if (bigmod_nat_is_odd(u) == 1 && bigmod_nat_is_odd(&v) == 1) {
            if (nat_cmp_geq(&v, u) == 0) {
                (void)nat_sub(u, &v);
                bigmod_nat_add(A, &C, &mod_m);
                bigmod_nat_add(&B, &D, &mod_a);
            } else {
                (void)nat_sub(&v, u);
                bigmod_nat_add(&C, A, &mod_m);
                bigmod_nat_add(&D, &B, &mod_a);
            }
        }

        /* Exactly one of u and v is now even. */
        if (bigmod_nat_is_odd(u) == bigmod_nat_is_odd(&v))
            panic_str(BURROW_S(
                "bigmod: internal error: u and v are not in the expected state"));

        /* Halve the even one and adjust the corresponding coefficient. */
        if (bigmod_nat_is_odd(u) == 0) {
            rshift1(u, 0);
            if (bigmod_nat_is_odd(A) == 1 || bigmod_nat_is_odd(&B) == 1) {
                rshift1(A, nat_add(A, m));
                rshift1(&B, nat_add(&B, a));
            } else {
                rshift1(A, 0);
                rshift1(&B, 0);
            }
        } else { /* v.IsOdd() == no */
            rshift1(&v, 0);
            if (bigmod_nat_is_odd(&C) == 1 || bigmod_nat_is_odd(&D) == 1) {
                rshift1(&C, nat_add(&C, m));
                rshift1(&D, nat_add(&D, a));
            } else {
                rshift1(&C, 0);
                rshift1(&D, 0);
            }
        }

        if (bigmod_nat_is_zero(&v) == 1)
            break;
    }
    bigmod_nat_free(&v);
    bigmod_nat_free(&B);
    bigmod_nat_free(&C);
    bigmod_nat_free(&D);
    return NULL;
}

bool bigmod_nat_inverse_var_time(BigmodNat *x, const BigmodNat *a,
                                 const BigmodModulus *m) {
    BigmodNat u, A;
    bigmod_nat_init(&u, NULL);
    bigmod_nat_init(&A, NULL);
    bool ok = extended_gcd(a, &m->nat, &u, &A) == NULL && bigmod_nat_is_one(&u) == 1;
    if (ok)
        bigmod_nat_set(x, &A);
    bigmod_nat_free(&u);
    bigmod_nat_free(&A);
    return ok;
}

BigmodNat *bigmod_nat_gcd_var_time(BigmodNat *x, const BigmodNat *a, const BigmodNat *b,
                                   Error *err) {
    BigmodNat u, A;
    bigmod_nat_init(&u, NULL);
    bigmod_nat_init(&A, NULL);
    const char *msg = extended_gcd(a, b, &u, &A);
    if (msg == NULL)
        bigmod_nat_set(x, &u);
    bigmod_nat_free(&u);
    bigmod_nat_free(&A);
    return msg != NULL ? bigmod_error(err, msg) : x;
}

BigmodNat *bigmod_nat_shift_right_by_one(BigmodNat *x) {
    rshift1(x, 0);
    return x;
}

Uint bigmod_nat_div_short_var_time(BigmodNat *x, Uint y) {
    if (y == 0)
        panic_str(BURROW_S("bigmod: division by zero"));

    Uint r = 0;
    for (Int i = x->len - 1; i >= 0; i--)
        x->limbs[i] = bits_div(r, x->limbs[i], y, &r);
    return r;
}
