/* Derived from Go's src/crypto/mldsa/mldsa_test.go, mldsa_wycheproof_test.go,
 * src/crypto/internal/fips140/mldsa/mldsa_test.go and field_test.go.
 * Go source: go1.27.1.
 *
 * The Wycheproof vectors Go fetches as a module, the ACVP known answer tests and
 * the messages of BenchmarkSign are in mldsa_test_gen.h, which
 * tools/gen-mldsa-tests.sh writes. The signatures and public keys the tests only
 * compare against are there as their SHA-256.
 *
 * Go's crypto/mldsa and its FIPS module are separate packages, and both have a
 * TestAccumulated. The one of the field arithmetic is TestFieldAccumulated here.
 * Go's tests see the rejections of a signature through a package variable,
 * and here the same callback is an argument of the functions that sign. Go's
 * TestUninitialized checks that nothing panics, which a C test cannot recover
 * from anyway, so it checks the errors. TestAllocations counts Go's heap
 * allocations and has no counterpart. BenchmarkSign goes through its messages
 * in order where Go shuffles them first. TestCAST and the tests after it are
 * burrow's, for the self test, the errors, the semi-expanded keys, the signer
 * and the allocator.
 *
 * Copyright 2025 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/crypto.h"
#include "burrow/crypto/mldsa.h"
#include "burrow/crypto/sha256.h"
#include "burrow/crypto/sha3.h"
#include "burrow/encoding/hex.h"
#include "burrow/mem/heap.h"
#include "burrow/slices.h"

#include "../src/crypto/mldsa_internal.h"

#include "mldsa_test_gen.h"

#include <stdint.h>
#include <string.h>

static Slice bs(void *p, Int n) {
    return slice_from(p, n, n, TYPE_BYTE);
}

/* The bytes of s, borrowed. */
static Slice cs(const char *s) {
    return bs((void *)(uintptr_t)s, (Int)strlen(s));
}

static Slice unhex(Alloc *a, const char *s) {
    Error err = BURROW_NO_ERROR;
    Slice b = hex_decode_string(a, str_from_cstr(s), &err);
    if (BURROW_FAILED(err))
        panic_str(BURROW_S("invalid hex"));
    return b;
}

/* A hex string the generator has cut into parts, as C99 can only take so much
 * in one literal. */
static Slice unhex_parts(Alloc *a, const char *const parts[GEN_MLDSA_PARTS]) {
    Slice b[GEN_MLDSA_PARTS];
    Int n = 0;
    while (n < GEN_MLDSA_PARTS && parts[n] != NULL) {
        b[n] = unhex(a, parts[n]);
        n++;
    }
    if (n == 1)
        return b[0];
    return slices_concat(a, b, n);
}

/* Whether the SHA-256 of b is the hex in want, in either case. */
static bool sha256_is(Slice b, const char *want) {
    static const char digits[] = "0123456789abcdef";
    Sha256Sum256Ret h = sha256_sum256(b);
    if (strlen(want) != 2 * SHA256_SIZE)
        return false;
    for (int i = 0; i < 2 * SHA256_SIZE; i++) {
        char c = want[i];
        if (c >= 'A' && c <= 'F')
            c = (char)(c - 'A' + 'a');
        int nibble = i % 2 == 0 ? h.a[i / 2] >> 4 : h.a[i / 2] & 15;
        if (c != digits[nibble])
            return false;
    }
    return true;
}

static Str str_of(Slice b) {
    return str_from_bytes(b.p, b.len);
}

static const MldsaParameters *params_of(int k) {
    switch (k) {
    case 44:
        return mldsa_mldsa44();
    case 65:
        return mldsa_mldsa65();
    case 87:
        return mldsa_mldsa87();
    default:
        panic_str(BURROW_S("unknown parameter set"));
    }
}

static const MldsaPublicKey *pub_of(const MldsaPrivateKey *sk) {
    return mldsa_private_key_public_key(sk);
}

static CryptoSignerOpts no_opts(void) {
    CryptoSignerOpts o = {0};
    return o;
}

static const CryptoHash mu_hash = CRYPTO_MLDSA_MU;
static const CryptoHash sha256_hash = CRYPTO_SHA256;
static const CryptoHash zero_hash = 0;

static void want_error(TestingT *t, const char *what, Error err, const char *text) {
    testing_t_helper(t);
    if (!BURROW_FAILED(err))
        testing_t_errorf_v(t, "%s: no error, want %s", what, text);
    else if (!str_eq(error_text(err), str_from_cstr(text)))
        testing_t_errorf_v(t, "%s: error %s, want %s", what, error_text(err), text);
}

/* The rejections a signature went through, Go's map reached. */
typedef struct {
    bool z, r0, ct0, h;
} Reached;

static void reached_fn(void *env, Str reason) {
    Reached *r = env;
    if (str_eq(reason, BURROW_S("z")))
        r->z = true;
    else if (str_eq(reason, BURROW_S("r0")))
        r->r0 = true;
    else if (str_eq(reason, BURROW_S("ct0")))
        r->ct0 = true;
    else if (str_eq(reason, BURROW_S("h")))
        r->h = true;
    else
        panic_str(BURROW_S("unknown rejection reason"));
}

static void check_reached(TestingT *t, const Reached *r, bool want_ct0) {
    testing_t_helper(t);
    if (!r->z)
        testing_t_errorf_v(t, "Rejection path %s not hit", "z");
    if (!r->r0)
        testing_t_errorf_v(t, "Rejection path %s not hit", "r0");
    if (want_ct0 && !r->ct0)
        testing_t_errorf_v(t, "Rejection path %s not hit", "ct0");
    if (!r->h)
        testing_t_errorf_v(t, "Rejection path %s not hit", "h");
}

/* ------------------------------------------------------------------ field */

#define Q ((uint32_t)MLDSA_Q)

typedef struct {
    uint32_t v;
    MldsaFieldElement m;
} Interesting;

/* q is large enough that all q × q inputs cannot be tried, so when there are
 * two inputs one side goes over [0, q) and the other over these. */
static int interesting_values(Interesting out[18]) {
    if (testing_short()) {
        out[0] = (Interesting){Q - 1, MLDSA_MINUS_ONE};
        return 1;
    }
    static const uint32_t vs[] = {0, 1, 2, 3, Q - 3, Q - 2, Q - 1, Q / 2, (Q + 1) / 2};
    int n = 0;
    for (size_t i = 0; i < sizeof vs / sizeof vs[0]; i++) {
        MldsaFieldElement m;
        (void)burrow__mldsa_field_to_montgomery(vs[i], &m);
        out[n++] = (Interesting){vs[i], m};
        /* Also values with an interesting Montgomery representation. */
        out[n++] = (Interesting){burrow__mldsa_field_from_montgomery(vs[i]), vs[i]};
    }
    return n;
}

static MldsaFieldElement to_mont(uint32_t a) {
    MldsaFieldElement m = 0;
    (void)burrow__mldsa_field_to_montgomery(a, &m);
    return m;
}

static void TestToFromMontgomery(TestingT *t) {
    for (uint32_t a = 0; a < Q; a++) {
        MldsaFieldElement m;
        if (!burrow__mldsa_field_to_montgomery(a, &m))
            testing_t_fatalf_v(t, "fieldToMontgomery(%d) returned an error", (Int)a);
        MldsaFieldElement exp = (MldsaFieldElement)(((uint64_t)a * MLDSA_R) % Q);
        if (m != exp)
            testing_t_fatalf_v(t, "fieldToMontgomery(%d) = %d, expected %d", (Int)a,
                               (Int)m, (Int)exp);
        uint32_t got = burrow__mldsa_field_from_montgomery(m);
        if (got != a)
            testing_t_fatalf_v(t,
                               "fieldFromMontgomery(fieldToMontgomery(%d)) = %d, "
                               "expected %d",
                               (Int)a, (Int)got, (Int)a);
    }
}

static void TestFieldAdd(TestingT *t) {
    Interesting iv[18];
    int n = interesting_values(iv);
    for (int i = 0; i < n; i++) {
        for (MldsaFieldElement b = 0; b < Q; b++) {
            MldsaFieldElement got = burrow__mldsa_field_add(iv[i].m, b);
            MldsaFieldElement exp = (iv[i].m + b) % Q;
            if (got != exp)
                testing_t_fatalf_v(t, "%d + %d = %d, expected %d", (Int)iv[i].m, (Int)b,
                                   (Int)got, (Int)exp);
        }
    }
}

static void TestFieldSub(TestingT *t) {
    Interesting iv[18];
    int n = interesting_values(iv);
    for (int i = 0; i < n; i++) {
        for (MldsaFieldElement b = 0; b < Q; b++) {
            MldsaFieldElement got = burrow__mldsa_field_sub(iv[i].m, b);
            MldsaFieldElement exp = (iv[i].m + Q - b) % Q;
            if (got != exp)
                testing_t_fatalf_v(t, "%d - %d = %d, expected %d", (Int)iv[i].m, (Int)b,
                                   (Int)got, (Int)exp);
        }
    }
}

static void TestFieldSubToMontgomery(TestingT *t) {
    Interesting iv[18];
    int n = interesting_values(iv);
    for (int i = 0; i < n; i++) {
        for (uint32_t b = 0; b < Q; b++) {
            MldsaFieldElement got = burrow__mldsa_field_sub_to_montgomery(iv[i].v, b);
            uint32_t diff = (iv[i].v + Q - b) % Q;
            MldsaFieldElement exp = (MldsaFieldElement)(((uint64_t)diff * MLDSA_R) % Q);
            if (got != exp)
                testing_t_fatalf_v(t, "fieldSubToMontgomery(%d, %d) = %d, expected %d",
                                   (Int)iv[i].v, (Int)b, (Int)got, (Int)exp);
        }
    }
}

static void TestFieldReduceOnce(TestingT *t) {
    for (uint32_t a = 0; a < 2 * Q; a++) {
        MldsaFieldElement got = burrow__mldsa_field_reduce_once(a);
        uint32_t exp = a < Q ? a : a - Q;
        if (got != exp)
            testing_t_fatalf_v(t, "fieldReduceOnce(%d) = %d, expected %d", (Int)a,
                               (Int)got, (Int)exp);
    }
}

static void TestFieldMul(TestingT *t) {
    Interesting iv[18];
    int n = interesting_values(iv);
    for (int i = 0; i < n; i++) {
        for (MldsaFieldElement b = 0; b < Q; b++) {
            uint32_t got = burrow__mldsa_field_from_montgomery(
                burrow__mldsa_field_montgomery_mul(iv[i].m, b));
            uint32_t exp = (uint32_t)(((uint64_t)iv[i].v *
                                       burrow__mldsa_field_from_montgomery(b)) %
                                      Q);
            if (got != exp)
                testing_t_fatalf_v(t, "%d * %d = %d, expected %d", (Int)iv[i].v, (Int)b,
                                   (Int)got, (Int)exp);
        }
    }
}

static void TestFieldToMontgomeryOverflow(TestingT *t) {
    /* fieldToMontgomery should reject inputs ≥ q. */
    static const uint32_t inputs[] = {
        Q,        Q + 1,          Q + 2,          (1u << 23) - 1,
        1u << 23, Q + (1u << 23), Q + (1u << 31), UINT32_MAX,
    };
    for (size_t i = 0; i < sizeof inputs / sizeof inputs[0]; i++) {
        MldsaFieldElement m;
        if (burrow__mldsa_field_to_montgomery(inputs[i], &m))
            testing_t_fatalf_v(t, "fieldToMontgomery(%d) did not return an error",
                               (Int)inputs[i]);
    }
}

static void TestFieldMulSub(TestingT *t) {
    Interesting iv[18];
    int n = interesting_values(iv);
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            for (int k = 0; k < n; k++) {
                uint32_t got = burrow__mldsa_field_from_montgomery(
                    burrow__mldsa_field_montgomery_mul_sub(iv[i].m, iv[j].m, iv[k].m));
                uint32_t exp =
                    (uint32_t)(((uint64_t)iv[i].v * ((uint64_t)iv[j].v + Q - iv[k].v)) %
                               Q);
                if (got != exp)
                    testing_t_fatalf_v(t, "%d * (%d - %d) = %d, expected %d",
                                       (Int)iv[i].v, (Int)iv[j].v, (Int)iv[k].v,
                                       (Int)got, (Int)exp);
            }
        }
    }
}

static void TestFieldAddMul(TestingT *t) {
    Interesting iv[18];
    int n = interesting_values(iv);
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            for (int k = 0; k < n; k++) {
                for (int l = 0; l < n; l++) {
                    uint32_t got = burrow__mldsa_field_from_montgomery(
                        burrow__mldsa_field_montgomery_add_mul(iv[i].m, iv[j].m,
                                                               iv[k].m, iv[l].m));
                    uint32_t exp = (uint32_t)(((uint64_t)iv[i].v * iv[j].v +
                                               (uint64_t)iv[k].v * iv[l].v) %
                                              Q);
                    if (got != exp)
                        testing_t_fatalf_v(t, "%d + %d * %d = %d, expected %d",
                                           (Int)iv[i].v, (Int)iv[j].v, (Int)iv[k].v,
                                           (Int)got, (Int)exp);
                }
            }
        }
    }
}

static uint8_t bit_rev8(uint8_t n) {
    uint8_t r = 0;
    for (int i = 0; i < 8; i++)
        r = (uint8_t)(r | (((n >> i) & 1) << (7 - i)));
    return r;
}

static int32_t centered_mod(uint32_t x, uint32_t m) {
    x = x % m;
    if (x > m / 2)
        return (int32_t)x - (int32_t)m;
    return (int32_t)x;
}

static uint32_t reduce_mod_q(int32_t x) {
    x %= (int32_t)Q;
    if (x < 0)
        return (uint32_t)(x + (int32_t)Q);
    return (uint32_t)x;
}

static void TestCenteredMod(TestingT *t) {
    for (uint32_t x = 0; x < 2 * Q; x++) {
        int32_t got = centered_mod(x, Q);
        if (reduce_mod_q(got) != x % Q)
            testing_t_fatalf_v(t,
                               "CenteredMod(%d) = %d, which is not congruent to %d mod "
                               "%d",
                               (Int)x, (Int)got, (Int)x, (Int)Q);
    }
    for (uint32_t x = 0; x < Q; x++) {
        int32_t got = burrow__mldsa_field_centered_mod(to_mont(x));
        int32_t exp = centered_mod(x, Q);
        if (got != exp)
            testing_t_fatalf_v(t, "fieldCenteredMod(%d) = %d, expected %d", (Int)x,
                               (Int)got, (Int)exp);
    }
}

static void TestInfinityNorm(TestingT *t) {
    for (uint32_t x = 0; x < Q; x++) {
        uint32_t got = burrow__mldsa_field_infinity_norm(to_mont(x));
        int32_t exp = centered_mod(x, Q);
        if (exp < 0)
            exp = -exp;
        if (got != (uint32_t)exp)
            testing_t_fatalf_v(t, "fieldInfinityNorm(%d) = %d, expected %d", (Int)x,
                               (Int)got, (Int)exp);
    }
}

static void TestConstants(TestingT *t) {
    if (burrow__mldsa_field_from_montgomery(MLDSA_ONE) != 1)
        testing_t_errorf_v(t, "one constant incorrect");
    if (burrow__mldsa_field_from_montgomery(MLDSA_MINUS_ONE) != Q - 1)
        testing_t_errorf_v(t, "minusOne constant incorrect");
    if (burrow__mldsa_field_infinity_norm(MLDSA_ONE) != 1)
        testing_t_errorf_v(t, "one infinity norm incorrect");
    if (burrow__mldsa_field_infinity_norm(MLDSA_MINUS_ONE) != 1)
        testing_t_errorf_v(t, "minusOne infinity norm incorrect");

    if (MLDSA_MLDSA44_PUBLIC_KEY_SIZE != burrow__mldsa_pub_key_size(mldsa_mldsa44()))
        testing_t_errorf_v(t, "PublicKeySize44 constant incorrect");
    if (MLDSA_MLDSA65_PUBLIC_KEY_SIZE != burrow__mldsa_pub_key_size(mldsa_mldsa65()))
        testing_t_errorf_v(t, "PublicKeySize65 constant incorrect");
    if (MLDSA_MLDSA87_PUBLIC_KEY_SIZE != burrow__mldsa_pub_key_size(mldsa_mldsa87()))
        testing_t_errorf_v(t, "PublicKeySize87 constant incorrect");
    if (MLDSA_MLDSA44_SIGNATURE_SIZE != burrow__mldsa_sig_size(mldsa_mldsa44()))
        testing_t_errorf_v(t, "SignatureSize44 constant incorrect");
    if (MLDSA_MLDSA65_SIGNATURE_SIZE != burrow__mldsa_sig_size(mldsa_mldsa65()))
        testing_t_errorf_v(t, "SignatureSize65 constant incorrect");
    if (MLDSA_MLDSA87_SIGNATURE_SIZE != burrow__mldsa_sig_size(mldsa_mldsa87()))
        testing_t_errorf_v(t, "SignatureSize87 constant incorrect");
}

static void TestPower2Round(TestingT *t) {
    for (uint32_t x = 0; x < Q; x++) {
        uint16_t t1;
        MldsaFieldElement t0;
        burrow__mldsa_power2_round(to_mont(x), &t1, &t0);
        MldsaFieldElement hi;
        if (!burrow__mldsa_field_to_montgomery((uint32_t)t1 << 13, &hi))
            testing_t_fatalf_v(t,
                               "power2Round(%d): failed to convert high part to "
                               "Montgomery",
                               (Int)x);
        uint32_t r =
            burrow__mldsa_field_from_montgomery(burrow__mldsa_field_add(hi, t0));
        if (r != x)
            testing_t_fatalf_v(t,
                               "power2Round(%d) = (%d, %d), which reconstructs to %d, "
                               "expected %d",
                               (Int)x, (Int)t1, (Int)t0, (Int)r, (Int)x);
    }
}

/* Decompose as FIPS 204 has it, for γ₂ = (q - 1) / den. */
static uint32_t spec_decompose(MldsaFieldElement rr, uint32_t den, int32_t *r0_out) {
    uint32_t r = burrow__mldsa_field_from_montgomery(rr);
    uint32_t gamma2 = (Q - 1) / den;
    int32_t r0 = centered_mod(r, 2 * gamma2);
    int32_t diff = (int32_t)r - r0;
    if (diff == (int32_t)Q - 1) {
        *r0_out = r0 - 1;
        return 0;
    }
    if (diff < 0 || (uint32_t)diff % gamma2 != 0)
        panic_str(BURROW_S("mldsa: internal error: invalid decomposition"));
    *r0_out = r0;
    return (uint32_t)diff / (2 * gamma2);
}

static void decompose(void *env, TestingT *t) {
    uint32_t den = (uint32_t)(uintptr_t)env;
    uint32_t gamma2 = (Q - 1) / den;
    for (uint32_t x = 0; x < Q; x++) {
        MldsaFieldElement rr = to_mont(x);
        int32_t r0;
        uint32_t r1 = spec_decompose(rr, den, &r0);

        /* Check that SpecDecompose is correct: r ≡ r1 * (2 * γ2) + r0 mod q. */
        uint32_t reconstructed = reduce_mod_q((int32_t)(r1 * 2 * gamma2) + r0);
        if (reconstructed != x)
            testing_t_fatalf_v(t,
                               "SpecDecompose(%d) = (%d, %d), which reconstructs to "
                               "%d, expected %d",
                               (Int)x, (Int)r1, (Int)r0, (Int)reconstructed, (Int)x);

        uint8_t got_r1;
        int32_t got_r0;
        if (den == 88) {
            got_r1 = burrow__mldsa_decompose88(rr, &got_r0);
            if (got_r1 > 43)
                testing_t_fatalf_v(t, "decompose88(%d) returned r1 = %d, out of range",
                                   (Int)x, (Int)got_r1);
        } else {
            got_r1 = burrow__mldsa_decompose32(rr, &got_r0);
            if (got_r1 > 15)
                testing_t_fatalf_v(t, "decompose32(%d) returned r1 = %d, out of range",
                                   (Int)x, (Int)got_r1);
        }
        if (got_r1 != r1)
            testing_t_fatalf_v(t, "highBits(%d) = %d, expected %d", (Int)x, (Int)got_r1,
                               (Int)r1);
        if (got_r0 != r0)
            testing_t_fatalf_v(t, "lowBits(%d) = %d, expected %d", (Int)x, (Int)got_r0,
                               (Int)r0);
    }
}

static void TestDecompose(TestingT *t) {
    testing_t_run(t, BURROW_S("ML-DSA-44"),
                  BURROW_FN(TestingTFunc, decompose, (void *)(uintptr_t)88));
    testing_t_run(t, BURROW_S("ML-DSA-65,87"),
                  BURROW_FN(TestingTFunc, decompose, (void *)(uintptr_t)32));
}

static void TestZetas(TestingT *t) {
    for (int k = 0; k < 256; k++) {
        /* ζ^BitRev₈(k) mod q */
        uint64_t exp = 1, base = 1753;
        for (unsigned e = bit_rev8((uint8_t)k); e != 0; e >>= 1) {
            if (e & 1)
                exp = exp * base % Q;
            base = base * base % Q;
        }
        uint32_t got = burrow__mldsa_field_from_montgomery(burrow__mldsa_zetas[k]);
        if (got != exp)
            testing_t_errorf_v(t, "zetas[%d] = %d, expected %d", k, (Int)got, (Int)exp);
    }
}

/* v in decimal and a newline at p, as fmt.Fprintf(o, "%d\n", v). */
static char *put_int(char *p, int64_t v) {
    char tmp[24];
    int n = 0;
    uint64_t u = v < 0 ? (uint64_t)(-v) : (uint64_t)v;
    if (v < 0)
        *p++ = '-';
    do {
        tmp[n++] = (char)('0' + u % 10);
        u /= 10;
    } while (u != 0);
    while (n > 0)
        *p++ = tmp[--n];
    *p++ = '\n';
    return p;
}

/* Go's internal TestAccumulated: the SHAKE128 of 12 values for every r in
 * [0, q), as decimals one to a line. They are r mod± q, ‖r‖∞, the two parts of
 * Power2Round(r), and for γ₂ = (q - 1) / 88 and then (q - 1) / 32 HighBits(r),
 * UseHint(1, r), LowBits(r) and ‖LowBits(r)‖∞. */
static void TestFieldAccumulated(TestingT *t) {
    if (testing_short())
        testing_t_skip_v(t, "skipping accumulated test in short mode");
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Sha3SHAKE *o = sha3_new_shake128(a);
    char line[12 * 24];
    for (uint32_t x = 0; x < Q; x++) {
        MldsaFieldElement r = to_mont(x);
        char *p = line;
        p = put_int(p, burrow__mldsa_field_centered_mod(r));
        p = put_int(p, burrow__mldsa_field_infinity_norm(r));

        uint16_t hi;
        MldsaFieldElement lo;
        burrow__mldsa_power2_round(r, &hi, &lo);
        p = put_int(p, hi);
        p = put_int(p, burrow__mldsa_field_from_montgomery(lo));

        int32_t r0;
        uint8_t r1 = burrow__mldsa_decompose88(r, &r0);
        uint8_t r1x = burrow__mldsa_high_bits88(burrow__mldsa_field_from_montgomery(r));
        if (r1x != r1)
            testing_t_fatalf_v(t, "highBits88(%d) = %d, expected %d", (Int)x, (Int)r1x,
                               (Int)r1);
        uint8_t r1h0 = burrow__mldsa_use_hint88(r, 0);
        if (r1h0 != r1)
            testing_t_fatalf_v(t, "useHint88(%d, 0) = %d, expected %d", (Int)x,
                               (Int)r1h0, (Int)r1);
        p = put_int(p, r1);
        p = put_int(p, burrow__mldsa_use_hint88(r, 1));
        p = put_int(p, r0);
        p = put_int(p, burrow__mldsa_constant_time_abs(r0));

        r1 = burrow__mldsa_decompose32(r, &r0);
        r1x = burrow__mldsa_high_bits32(burrow__mldsa_field_from_montgomery(r));
        if (r1x != r1)
            testing_t_fatalf_v(t, "highBits32(%d) = %d, expected %d", (Int)x, (Int)r1x,
                               (Int)r1);
        r1h0 = burrow__mldsa_use_hint32(r, 0);
        if (r1h0 != r1)
            testing_t_fatalf_v(t, "useHint32(%d, 0) = %d, expected %d", (Int)x,
                               (Int)r1h0, (Int)r1);
        p = put_int(p, r1);
        p = put_int(p, burrow__mldsa_use_hint32(r, 1));
        p = put_int(p, r0);
        p = put_int(p, burrow__mldsa_constant_time_abs(r0));
        sha3_shake_write(o, bs(line, p - line), NULL);
    }

    /* The expected value is documented at https://c2sp.org/CCTV/ML-DSA, and
     * tested against https://github.com/FiloSottile/mldsa-py. */
    Byte sum[32];
    sha3_shake_read(o, bs(sum, sizeof sum), NULL);
    Str got = hex_encode_to_string(a, bs(sum, sizeof sum));
    const char *expected =
        "f930663417278156ab05d940294a77210a809c924d8ab63ec72f4526247602c7";
    if (!str_eq(got, str_from_cstr(expected)))
        testing_t_errorf_v(t, "got %s, expected %s", got, expected);
    arena_free(&ar);
}

#undef Q

/* ------------------------------------------------------------ public API */

typedef struct {
    const char *name;
    int params;
    Int n;
    const char *expected;
} AccumulatedCase;

/* TestAccumulated accumulates 10k (or 100) random vectors and checks the hash
 * of the result, to avoid checking in megabytes of test vectors. */
static void accumulated(void *env, TestingT *t) {
    const AccumulatedCase *c = env;
    const MldsaParameters *params = params_of(c->params);
    Arena keep, ar;
    arena_init(&keep, NULL, 0);
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Sha3SHAKE *s = sha3_new_shake128(arena_allocator(&keep));
    Sha3SHAKE *o = sha3_new_shake128(arena_allocator(&keep));
    Byte seed[MLDSA_PRIVATE_KEY_SIZE];
    Slice msg = slice_nil(TYPE_BYTE);

    for (Int i = 0; i < c->n; i++) {
        Error err = BURROW_NO_ERROR;
        sha3_shake_read(s, bs(seed, sizeof seed), NULL);
        MldsaPrivateKey *dk =
            mldsa_new_private_key(a, params, bs(seed, sizeof seed), &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "NewPrivateKey: %s", error_text(err));
        Slice pk = mldsa_public_key_bytes(pub_of(dk), a);
        sha3_shake_write(o, pk, NULL);
        Slice sig = mldsa_private_key_sign_deterministic(dk, a, msg, no_opts(), &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "SignDeterministic: %s", error_text(err));
        sha3_shake_write(o, sig, NULL);
        MldsaPublicKey *pub = mldsa_new_public_key(a, params, pk, &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "NewPublicKey: %s", error_text(err));
        if (!burrow__mldsa_public_key_identical(pub, pub_of(dk)))
            testing_t_fatalf_v(t, "public key mismatch");
        err = mldsa_verify(pub_of(dk), msg, sig, NULL);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "Verify: %s", error_text(err));
        arena_reset(&ar);
    }

    Byte sum[32];
    sha3_shake_read(o, bs(sum, sizeof sum), NULL);
    Str got = hex_encode_to_string(a, bs(sum, sizeof sum));
    if (!str_eq(got, str_from_cstr(c->expected)))
        testing_t_errorf_v(t, "got %s, expected %s", got, c->expected);
    arena_free(&ar);
    arena_free(&keep);
}

static void TestAccumulated(TestingT *t) {
    static const AccumulatedCase cases[] = {
        {"ML-DSA-44/100", 44, 100,
         "d51148e1f9f4fa1a723a6cf42e25f2a99eb5c1b378b3d2dbbd561b1203beeae4"},
        {"ML-DSA-65/100", 65, 100,
         "8358a1843220194417cadbc2651295cd8fc65125b5a5c1a239a16dc8b57ca199"},
        {"ML-DSA-87/100", 87, 100,
         "8c3ad714777622b8f21ce31bb35f71394f23bc0fcf3c78ace5d608990f3b061b"},
        {"ML-DSA-44/10k", 44, 10000,
         "e7fd21f6a59bcba60d65adc44404bb29a7c00e5d8d3ec06a732c00a306a7d143"},
        {"ML-DSA-65/10k", 65, 10000,
         "5ff5e196f0b830c3b10a9eb5358e7c98a3a20136cb677f3ae3b90175c3ace329"},
        {"ML-DSA-87/10k", 87, 10000,
         "80a8cf39317f7d0be0e24972c51ac152bd2a3e09bc0c32ce29dd82c4e7385e60"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        if (cases[i].n > 100 && testing_short())
            break;
        testing_t_run(
            t, str_from_cstr(cases[i].name),
            BURROW_FN(TestingTFunc, accumulated, (void *)(uintptr_t)&cases[i]));
    }
}

static void test_all_parameters(TestingT *t, void (*f)(void *, TestingT *)) {
    const MldsaParameters *all[] = {mldsa_mldsa44(), mldsa_mldsa65(), mldsa_mldsa87()};
    for (size_t i = 0; i < 3; i++)
        testing_t_run(t, mldsa_parameters_string(all[i]),
                      BURROW_FN(TestingTFunc, f, (void *)(uintptr_t)all[i]));
}

static MldsaPrivateKey *must_generate(TestingT *t, const MldsaParameters *params,
                                      Alloc *a) {
    Error err = BURROW_NO_ERROR;
    MldsaPrivateKey *sk = mldsa_generate_key(a, params, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "GenerateKey: %s", error_text(err));
    return sk;
}

static void generate_key(void *env, TestingT *t) {
    const MldsaParameters *params = env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    MldsaPrivateKey *k1 = must_generate(t, params, a);
    MldsaPrivateKey *k2 = must_generate(t, params, a);
    if (mldsa_private_key_equal(k1, BURROW_ANY(TYPE_MLDSA_PRIVATE_KEY, k2)))
        testing_t_errorf_v(t, "two generated keys are equal");
    Error err = BURROW_NO_ERROR;
    MldsaPrivateKey *k1x =
        mldsa_new_private_key(a, params, mldsa_private_key_bytes(k1, a), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "NewPrivateKey: %s", error_text(err));
    if (!mldsa_private_key_equal(k1, BURROW_ANY(TYPE_MLDSA_PRIVATE_KEY, k1x)))
        testing_t_errorf_v(t, "generated key and re-parsed key are not equal");
    arena_free(&ar);
}

static void TestGenerateKey(TestingT *t) {
    test_all_parameters(t, generate_key);
}

static void TestParametersIdentity(TestingT *t) {
    /* Repeated calls return the same value, suitable for equality checks. */
    if (mldsa_mldsa44() != mldsa_mldsa44() || mldsa_mldsa65() != mldsa_mldsa65() ||
        mldsa_mldsa87() != mldsa_mldsa87())
        testing_t_errorf_v(t, "MLDSA*() returned different values across calls");
    if (mldsa_mldsa44() == mldsa_mldsa65() || mldsa_mldsa65() == mldsa_mldsa87() ||
        mldsa_mldsa44() == mldsa_mldsa87())
        testing_t_errorf_v(t, "distinct parameter sets compare equal");
}

/* μ = SHAKE256(SHAKE256(pk, 64) || 0x00 || ctxlen || ctx || msg, 64) per FIPS
 * 204, to drive the external μ signing path. */
static Slice compute_mu(Alloc *a, Slice pk, Str ctx, Slice msg) {
    Slice tr = sha3_sum_shake256(a, pk, 64);
    Sha3SHAKE *h = sha3_new_shake256(a);
    sha3_shake_write(h, tr, NULL);
    Byte head[2] = {0x00, (Byte)ctx.len};
    sha3_shake_write(h, bs(head, 2), NULL);
    sha3_shake_write(
        h, slice_from((void *)(uintptr_t)ctx.p, ctx.len, ctx.len, TYPE_BYTE), NULL);
    sha3_shake_write(h, msg, NULL);
    Slice out = slice_make(a, TYPE_BYTE, 64, 64);
    sha3_shake_read(h, out, NULL);
    return out;
}

/* A CryptoSignerOpts that is not an MldsaOptions, whose hash is *h, Go's
 * fakeSignerOpts. Its type only has to be some other one. */
static CryptoHash fake_hash_func(void *self) {
    return *(const CryptoHash *)self;
}

static const CryptoSignerOptsVT fake_opts_vt = {TYPE_INT, fake_hash_func};

static CryptoSignerOpts fake_opts(const CryptoHash *h) {
    CryptoSignerOpts o = {&fake_opts_vt, (void *)(uintptr_t)h};
    return o;
}

static void sign(void *env, TestingT *t) {
    const MldsaParameters *params = env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    MldsaPrivateKey *sk = must_generate(t, params, a);
    const MldsaPublicKey *pk = pub_of(sk);
    Slice msg = slices_clone(a, cs("test message"));
    MldsaOptions empty = {BURROW_STR_EMPTY};

    /* nil opts and &Options{} must be equivalent, and both interoperable with
     * nil or zero Verify opts. */
    Slice sig1 = mldsa_private_key_sign(sk, a, (IoReader){0}, msg, no_opts(), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Sign(nil opts): %s", error_text(err));
    if (sig1.len != mldsa_parameters_signature_size(params))
        testing_t_errorf_v(t, "len(sig) = %d, want %d", sig1.len,
                           mldsa_parameters_signature_size(params));
    if (BURROW_FAILED(err = mldsa_verify(pk, msg, sig1, NULL)))
        testing_t_errorf_v(t, "Verify of nil-opts signature with nil opts: %s",
                           error_text(err));
    if (BURROW_FAILED(err = mldsa_verify(pk, msg, sig1, &empty)))
        testing_t_errorf_v(t, "Verify of nil-opts signature with empty Options: %s",
                           error_text(err));

    Slice sig2 = mldsa_private_key_sign(sk, a, (IoReader){0}, msg,
                                        mldsa_options_as_signer_opts(&empty), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Sign(&Options{}): %s", error_text(err));
    if (BURROW_FAILED(err = mldsa_verify(pk, msg, sig2, NULL)))
        testing_t_errorf_v(t, "Verify of empty-Options signature with nil opts: %s",
                           error_text(err));

    /* Opts that are not an Options with a HashFunc of 0 also sign directly,
     * with an empty context. */
    Slice sig3 =
        mldsa_private_key_sign(sk, a, (IoReader){0}, msg, fake_opts(&zero_hash), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Sign(fakeSignerOpts{0}): %s", error_text(err));
    if (BURROW_FAILED(err = mldsa_verify(pk, msg, sig3, NULL)))
        testing_t_errorf_v(t, "Verify of fake-opts signature: %s", error_text(err));

    Slice sig4 = mldsa_private_key_sign(sk, a, (IoReader){0}, msg,
                                        crypto_hash_as_signer_opts(&zero_hash), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Sign(crypto.Hash(0)): %s", error_text(err));
    if (BURROW_FAILED(err = mldsa_verify(pk, msg, sig4, NULL)))
        testing_t_errorf_v(t, "Verify of Hash(0)-opts signature: %s", error_text(err));

    /* A wrong HashFunc must produce errInvalidSignerOpts. */
    mldsa_private_key_sign(sk, a, (IoReader){0}, msg,
                           crypto_hash_as_signer_opts(&sha256_hash), &err);
    if (!BURROW_FAILED(err))
        testing_t_errorf_v(t, "Sign with crypto.SHA256 opts: want error, got nil");
    err = BURROW_NO_ERROR;
    mldsa_private_key_sign_deterministic(
        sk, a, msg, crypto_hash_as_signer_opts(&sha256_hash), &err);
    if (!BURROW_FAILED(err))
        testing_t_errorf_v(t,
                           "SignDeterministic with crypto.SHA256 opts: want error, got "
                           "nil");
    err = BURROW_NO_ERROR;

    /* SignDeterministic with nil and &Options{} must agree byte for byte. */
    Slice det_a = mldsa_private_key_sign_deterministic(sk, a, msg, no_opts(), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "SignDeterministic(nil): %s", error_text(err));
    Slice det_b = mldsa_private_key_sign_deterministic(
        sk, a, msg, mldsa_options_as_signer_opts(&empty), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "SignDeterministic(&Options{}): %s", error_text(err));
    if (!bytes_equal(det_a, det_b))
        testing_t_errorf_v(t, "SignDeterministic with nil and &Options{} differ");

    /* A different Context produces a different deterministic signature, and
     * verification with a mismatched context must fail. */
    MldsaOptions ctx = {BURROW_S("ctx")};
    Slice det_ctx = mldsa_private_key_sign_deterministic(
        sk, a, msg, mldsa_options_as_signer_opts(&ctx), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "SignDeterministic(ctx): %s", error_text(err));
    if (bytes_equal(det_ctx, det_a))
        testing_t_errorf_v(t,
                           "SignDeterministic with empty and non-empty context match");
    if (!BURROW_FAILED(mldsa_verify(pk, msg, det_ctx, NULL)))
        testing_t_errorf_v(
            t, "Verify of context signature with empty context: want error, "
               "got nil");
    if (BURROW_FAILED(err = mldsa_verify(pk, msg, det_ctx, &ctx)))
        testing_t_errorf_v(t, "Verify with matching context: %s", error_text(err));

    /* A Context over 255 bytes is rejected. */
    MldsaOptions long_ctx = {strings_repeat(a, BURROW_S("x"), 256)};
    mldsa_private_key_sign(sk, a, (IoReader){0}, msg,
                           mldsa_options_as_signer_opts(&long_ctx), &err);
    if (!BURROW_FAILED(err))
        testing_t_errorf_v(t, "Sign with 256-byte context: want error, got nil");
    err = BURROW_NO_ERROR;
    mldsa_private_key_sign_deterministic(sk, a, msg,
                                         mldsa_options_as_signer_opts(&long_ctx), &err);
    if (!BURROW_FAILED(err))
        testing_t_errorf_v(
            t, "SignDeterministic with 256-byte context: want error, got nil");
    err = BURROW_NO_ERROR;
    if (!BURROW_FAILED(mldsa_verify(pk, msg, det_a, &long_ctx)))
        testing_t_errorf_v(t, "Verify with 256-byte context: want error, got nil");

    /* A tampered signature must not verify. */
    Slice tampered = slices_clone(a, sig1);
    ((Byte *)tampered.p)[tampered.len / 2] ^= 0x01;
    if (!BURROW_FAILED(mldsa_verify(pk, msg, tampered, NULL)))
        testing_t_errorf_v(t, "Verify of tampered signature: want error, got nil");

    /* A modified message must not verify against the original signature. */
    Slice msg_tampered = slices_clone(a, msg);
    ((Byte *)msg_tampered.p)[0] ^= 0x01;
    if (!BURROW_FAILED(mldsa_verify(pk, msg_tampered, sig1, NULL)))
        testing_t_errorf_v(t, "Verify of modified message: want error, got nil");

    /* A signature from a different key must not verify. */
    MldsaPrivateKey *sk_other = must_generate(t, params, a);
    Slice sig_other =
        mldsa_private_key_sign_deterministic(sk_other, a, msg, no_opts(), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "SignDeterministic: %s", error_text(err));
    if (!BURROW_FAILED(mldsa_verify(pk, msg, sig_other, NULL)))
        testing_t_errorf_v(t,
                           "Verify of signature from a different key: want error, got "
                           "nil");
    arena_free(&ar);
}

static void TestSign(TestingT *t) {
    test_all_parameters(t, sign);
}

static void external_mu(void *env, TestingT *t) {
    const MldsaParameters *params = env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    MldsaPrivateKey *sk = must_generate(t, params, a);
    const MldsaPublicKey *pk = pub_of(sk);
    Slice pk_bytes = mldsa_public_key_bytes(pk, a);
    Slice msg = slices_clone(a, cs("hello mu"));
    CryptoSignerOpts mu_opts = crypto_hash_as_signer_opts(&mu_hash);

    static const char *const ctxs[] = {"", "ctx"};
    for (int i = 0; i < 2; i++) {
        MldsaOptions opts = {str_from_cstr(ctxs[i])};
        Slice mu = compute_mu(a, pk_bytes, opts.context, msg);
        Slice sig = mldsa_private_key_sign(sk, a, (IoReader){0}, mu, mu_opts, &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "Sign(MLDSAMu, ctx=%s): %s", ctxs[i],
                               error_text(err));
        if (BURROW_FAILED(err = mldsa_verify(pk, msg, sig, &opts)))
            testing_t_errorf_v(t, "Verify of MLDSAMu signature, ctx=%s: %s", ctxs[i],
                               error_text(err));

        Slice det = mldsa_private_key_sign_deterministic(sk, a, mu, mu_opts, &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "SignDeterministic(MLDSAMu, ctx=%s): %s", ctxs[i],
                               error_text(err));
        if (BURROW_FAILED(err = mldsa_verify(pk, msg, det, &opts)))
            testing_t_errorf_v(t,
                               "Verify of deterministic MLDSAMu signature, ctx=%s: %s",
                               ctxs[i], error_text(err));
        Slice det2 = mldsa_private_key_sign_deterministic(sk, a, mu, mu_opts, &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "SignDeterministic(MLDSAMu) second call: %s",
                               error_text(err));
        if (!bytes_equal(det, det2))
            testing_t_errorf_v(t, "SignDeterministic(MLDSAMu) is not deterministic");
    }

    /* μ worked out under one context must not verify under another. */
    MldsaOptions ctx_a = {BURROW_S("a")}, ctx_b = {BURROW_S("b")};
    Slice mu_a = compute_mu(a, pk_bytes, ctx_a.context, msg);
    Slice sig_a = mldsa_private_key_sign(sk, a, (IoReader){0}, mu_a, mu_opts, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Sign(MLDSAMu, ctx=a): %s", error_text(err));
    if (!BURROW_FAILED(mldsa_verify(pk, msg, sig_a, &ctx_b)))
        testing_t_errorf_v(t,
                           "Verify of MLDSAMu(ctx=a) signature with ctx=b: want error, "
                           "got nil");

    /* A tampered MLDSAMu signature must not verify. */
    Slice tampered = slices_clone(a, sig_a);
    ((Byte *)tampered.p)[tampered.len / 2] ^= 0x01;
    if (!BURROW_FAILED(mldsa_verify(pk, msg, tampered, &ctx_a)))
        testing_t_errorf_v(t,
                           "Verify of tampered MLDSAMu signature: want error, got nil");

    /* A μ of the wrong length must be rejected. */
    Byte short_mu[32] = {0};
    mldsa_private_key_sign(sk, a, (IoReader){0}, bs(short_mu, 32), mu_opts, &err);
    if (!BURROW_FAILED(err))
        testing_t_errorf_v(t, "Sign(MLDSAMu) with 32-byte input: want error, got nil");
    err = BURROW_NO_ERROR;
    mldsa_private_key_sign_deterministic(sk, a, bs(short_mu, 32), mu_opts, &err);
    if (!BURROW_FAILED(err))
        testing_t_errorf_v(t,
                           "SignDeterministic(MLDSAMu) with 32-byte input: want error, "
                           "got nil");
    arena_free(&ar);
}

static void TestExternalMu(TestingT *t) {
    test_all_parameters(t, external_mu);
}

typedef struct {
    int params;
    const char *name;
    Int pk_size, sig_size;
} PublicKeyCase;

static void public_key(void *env, TestingT *t) {
    const PublicKeyCase *c = env;
    const MldsaParameters *params = params_of(c->params);
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;

    Str name = mldsa_parameters_string(params);
    if (!str_eq(name, str_from_cstr(c->name)))
        testing_t_errorf_v(t, "Parameters.String() = %s, want %s", name, c->name);
    if (mldsa_parameters_public_key_size(params) != c->pk_size)
        testing_t_errorf_v(t, "Parameters.PublicKeySize() = %d, want %d",
                           mldsa_parameters_public_key_size(params), c->pk_size);
    if (mldsa_parameters_signature_size(params) != c->sig_size)
        testing_t_errorf_v(t, "Parameters.SignatureSize() = %d, want %d",
                           mldsa_parameters_signature_size(params), c->sig_size);

    MldsaPrivateKey *sk = must_generate(t, params, a);
    const MldsaPublicKey *pk = pub_of(sk);
    if (mldsa_public_key_parameters(pk) != params)
        testing_t_errorf_v(t, "PublicKey.Parameters() = %s, want %s",
                           mldsa_parameters_string(mldsa_public_key_parameters(pk)),
                           name);
    Slice pk_bytes = mldsa_public_key_bytes(pk, a);
    if (pk_bytes.len != mldsa_parameters_public_key_size(params))
        testing_t_errorf_v(t, "len(PublicKey.Bytes()) = %d, want %d", pk_bytes.len,
                           mldsa_parameters_public_key_size(params));
    Slice sk_bytes = mldsa_private_key_bytes(sk, a);
    if (sk_bytes.len != MLDSA_PRIVATE_KEY_SIZE)
        testing_t_errorf_v(t, "len(PrivateKey.Bytes()) = %d, want %d", sk_bytes.len,
                           (Int)MLDSA_PRIVATE_KEY_SIZE);

    /* Public() returns the same key as PublicKey(). */
    CryptoPublicKey any_pub = mldsa_private_key_public(sk);
    if (any_pub.t != TYPE_MLDSA_PUBLIC_KEY)
        testing_t_fatalf_v(t, "PrivateKey.Public() is a %s, want MldsaPublicKey",
                           any_pub.t != NULL ? any_pub.t->name : BURROW_S("nil"));
    if (!mldsa_public_key_equal(pk, any_pub))
        testing_t_errorf_v(t, "PrivateKey.Public() does not equal PublicKey()");

    /* Round trip through NewPrivateKey and NewPublicKey. */
    MldsaPrivateKey *sk2 = mldsa_new_private_key(a, params, sk_bytes, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "NewPrivateKey round-trip: %s", error_text(err));
    if (!mldsa_private_key_equal(sk, BURROW_ANY(TYPE_MLDSA_PRIVATE_KEY, sk2)))
        testing_t_errorf_v(t, "PrivateKey round-trip not equal");
    MldsaPublicKey *pk2 = mldsa_new_public_key(a, params, pk_bytes, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "NewPublicKey round-trip: %s", error_text(err));
    if (!mldsa_public_key_equal(pk, BURROW_ANY(TYPE_MLDSA_PUBLIC_KEY, pk2)))
        testing_t_errorf_v(t, "PublicKey round-trip not equal");
    arena_free(&ar);
}

static void TestPublicKey(TestingT *t) {
    static const PublicKeyCase cases[] = {
        {44, "ML-DSA-44", MLDSA_MLDSA44_PUBLIC_KEY_SIZE, MLDSA_MLDSA44_SIGNATURE_SIZE},
        {65, "ML-DSA-65", MLDSA_MLDSA65_PUBLIC_KEY_SIZE, MLDSA_MLDSA65_SIGNATURE_SIZE},
        {87, "ML-DSA-87", MLDSA_MLDSA87_PUBLIC_KEY_SIZE, MLDSA_MLDSA87_SIGNATURE_SIZE},
    };
    for (size_t i = 0; i < 3; i++)
        testing_t_run(
            t, str_from_cstr(cases[i].name),
            BURROW_FN(TestingTFunc, public_key, (void *)(uintptr_t)&cases[i]));
}

static void TestEqualWrongType(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    MldsaPrivateKey *sk = must_generate(t, mldsa_mldsa44(), a);
    Str not_a_key = BURROW_S("not a key");
    if (mldsa_private_key_equal(sk, BURROW_ANY(TYPE_STRING, &not_a_key)))
        testing_t_errorf_v(t, "PrivateKey.Equal(string) = true, want false");
    if (mldsa_private_key_equal(sk, BURROW_ANY(TYPE_MLDSA_PUBLIC_KEY, NULL)))
        testing_t_errorf_v(t, "PrivateKey.Equal(*PublicKey) = true, want false");
    if (mldsa_public_key_equal(pub_of(sk), BURROW_ANY(TYPE_STRING, &not_a_key)))
        testing_t_errorf_v(t, "PublicKey.Equal(string) = true, want false");
    if (mldsa_public_key_equal(pub_of(sk), BURROW_ANY(TYPE_MLDSA_PRIVATE_KEY, NULL)))
        testing_t_errorf_v(t, "PublicKey.Equal(*PrivateKey) = true, want false");

    /* Distinct keys are not Equal. */
    MldsaPrivateKey *sk2 = must_generate(t, mldsa_mldsa44(), a);
    if (mldsa_private_key_equal(sk, BURROW_ANY(TYPE_MLDSA_PRIVATE_KEY, sk2)))
        testing_t_errorf_v(t, "two random PrivateKeys are Equal");
    if (mldsa_public_key_equal(pub_of(sk), mldsa_private_key_public(sk2)))
        testing_t_errorf_v(t, "two random PublicKeys are Equal");
    arena_free(&ar);
}

static void TestInvalidParameters(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Byte zeros[MLDSA_MLDSA44_PUBLIC_KEY_SIZE] = {0};
    Error err = BURROW_NO_ERROR;
    mldsa_generate_key(a, NULL, &err);
    if (!BURROW_FAILED(err))
        testing_t_errorf_v(t, "GenerateKey(zero Parameters): want error, got nil");
    err = BURROW_NO_ERROR;
    mldsa_new_private_key(a, NULL, bs(zeros, MLDSA_PRIVATE_KEY_SIZE), &err);
    if (!BURROW_FAILED(err))
        testing_t_errorf_v(t, "NewPrivateKey(zero Parameters): want error, got nil");
    err = BURROW_NO_ERROR;
    mldsa_new_public_key(a, NULL, bs(zeros, MLDSA_MLDSA44_PUBLIC_KEY_SIZE), &err);
    if (!BURROW_FAILED(err))
        testing_t_errorf_v(t, "NewPublicKey(zero Parameters): want error, got nil");
    arena_free(&ar);
}

static void invalid_size(void *env, TestingT *t) {
    const MldsaParameters *params = env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Byte zeros[MLDSA_MLDSA87_PUBLIC_KEY_SIZE + 1] = {0};
    Int pk_size = mldsa_parameters_public_key_size(params);
    Error err = BURROW_NO_ERROR;

    mldsa_new_private_key(a, params, bs(zeros, MLDSA_PRIVATE_KEY_SIZE - 1), &err);
    if (!BURROW_FAILED(err))
        testing_t_errorf_v(t, "NewPrivateKey with short seed: want error, got nil");
    err = BURROW_NO_ERROR;
    mldsa_new_private_key(a, params, bs(zeros, MLDSA_PRIVATE_KEY_SIZE + 1), &err);
    if (!BURROW_FAILED(err))
        testing_t_errorf_v(t, "NewPrivateKey with long seed: want error, got nil");
    err = BURROW_NO_ERROR;
    mldsa_new_public_key(a, params, bs(zeros, pk_size - 1), &err);
    if (!BURROW_FAILED(err))
        testing_t_errorf_v(t, "NewPublicKey with short encoding: want error, got nil");
    err = BURROW_NO_ERROR;
    mldsa_new_public_key(a, params, bs(zeros, pk_size + 1), &err);
    if (!BURROW_FAILED(err))
        testing_t_errorf_v(t, "NewPublicKey with long encoding: want error, got nil");
    err = BURROW_NO_ERROR;

    MldsaPrivateKey *sk = must_generate(t, params, a);
    Slice msg = slices_clone(a, cs("test message"));
    Slice sig = mldsa_private_key_sign_deterministic(sk, a, msg, no_opts(), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "SignDeterministic: %s", error_text(err));
    if (!BURROW_FAILED(
            mldsa_verify(pub_of(sk), msg, slice_sub(sig, 0, sig.len - 1), NULL)))
        testing_t_errorf_v(t, "Verify with short signature: want error, got nil");
    Byte zero = 0;
    Slice longer = slices_concat(a, (Slice[]){sig, bs(&zero, 1)}, 2);
    if (!BURROW_FAILED(mldsa_verify(pub_of(sk), msg, longer, NULL)))
        testing_t_errorf_v(t, "Verify with long signature: want error, got nil");
    arena_free(&ar);
}

static void TestInvalidSize(TestingT *t) {
    test_all_parameters(t, invalid_size);

    /* An ML-DSA-65 public key encoding is rejected by ML-DSA-44, because the
     * lengths differ. */
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    MldsaPrivateKey *sk65 = must_generate(t, mldsa_mldsa65(), a);
    Error err = BURROW_NO_ERROR;
    mldsa_new_public_key(a, mldsa_mldsa44(), mldsa_public_key_bytes(pub_of(sk65), a),
                         &err);
    if (!BURROW_FAILED(err))
        testing_t_errorf_v(
            t, "NewPublicKey(MLDSA44, MLDSA65 encoding): want error, got nil");
    arena_free(&ar);
}

static void TestUninitialized(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;

    /* Verify with Go's zero PublicKey. */
    Slice msg = slices_clone(a, cs("attacker-controlled message"));
    Slice sig = slice_make(a, TYPE_BYTE, MLDSA_MLDSA44_SIGNATURE_SIZE,
                           MLDSA_MLDSA44_SIGNATURE_SIZE);
    MldsaPublicKey *zero_pk = burrow__mldsa_new_zero_public_key(a);
    MldsaOptions empty = {BURROW_STR_EMPTY}, ctx = {BURROW_S("ctx")};
    struct {
        const char *name;
        Slice sig;
        const MldsaOptions *opts;
    } vcases[] = {
        {"empty signature/nil opts", slice_nil(TYPE_BYTE), NULL},
        {"empty signature/empty opts", slice_nil(TYPE_BYTE), &empty},
        {"zero length signature", slice_sub(sig, 0, 0), NULL},
        {"full length signature", sig, NULL},
        {"full length signature/with context", sig, &ctx},
    };
    for (size_t i = 0; i < sizeof vcases / sizeof vcases[0]; i++) {
        if (!BURROW_FAILED(mldsa_verify(zero_pk, msg, vcases[i].sig, vcases[i].opts)))
            testing_t_errorf_v(t, "Verify/%s: accepted uninitialized PublicKey",
                               vcases[i].name);
        if (!BURROW_FAILED(mldsa_verify(NULL, msg, vcases[i].sig, vcases[i].opts)))
            testing_t_errorf_v(t, "Verify/%s: accepted a NULL PublicKey",
                               vcases[i].name);
    }

    /* Sign and SignDeterministic with Go's zero PrivateKey, and with NULL. */
    MldsaPrivateKey *zero_sk = burrow__mldsa_new_zero_private_key(a);
    Slice m = slices_clone(a, cs("message"));
    struct {
        const char *name;
        CryptoSignerOpts opts;
    } scases[] = {
        {"nil opts", no_opts()},
        {"empty opts", mldsa_options_as_signer_opts(&empty)},
        {"typed nil opts", mldsa_options_as_signer_opts(NULL)},
        {"mu opts", crypto_hash_as_signer_opts(&mu_hash)},
    };
    const MldsaPrivateKey *sks[] = {zero_sk, NULL};
    for (size_t k = 0; k < 2; k++) {
        for (size_t i = 0; i < sizeof scases / sizeof scases[0]; i++) {
            err = BURROW_NO_ERROR;
            mldsa_private_key_sign(sks[k], a, (IoReader){0}, m, scases[i].opts, &err);
            if (!BURROW_FAILED(err))
                testing_t_errorf_v(t, "Sign/%s: accepted zero-value PrivateKey",
                                   scases[i].name);
            err = BURROW_NO_ERROR;
            mldsa_private_key_sign_deterministic(sks[k], a, m, scases[i].opts, &err);
            if (!BURROW_FAILED(err))
                testing_t_errorf_v(t,
                                   "SignDeterministic/%s: accepted zero-value "
                                   "PrivateKey",
                                   scases[i].name);
        }
    }

    /* TypedNilOptions: a NULL *Options is the same as none. */
    err = BURROW_NO_ERROR;
    MldsaPrivateKey *sk = must_generate(t, mldsa_mldsa44(), a);
    Slice want = mldsa_private_key_sign_deterministic(sk, a, m, no_opts(), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "SignDeterministic(nil opts): %s", error_text(err));
    Slice got = mldsa_private_key_sign_deterministic(
        sk, a, m, mldsa_options_as_signer_opts(NULL), &err);
    if (BURROW_FAILED(err) || !bytes_equal(got, want))
        testing_t_errorf_v(t,
                           "SignDeterministic(typed-nil opts) != SignDeterministic(nil "
                           "opts)");
    err = BURROW_NO_ERROR;
    Slice s = mldsa_private_key_sign(sk, a, (IoReader){0}, m,
                                     mldsa_options_as_signer_opts(NULL), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Sign(typed-nil opts): %s", error_text(err));
    if (BURROW_FAILED(err = mldsa_verify(pub_of(sk), m, s, NULL)))
        testing_t_errorf_v(t,
                           "signature made with typed-nil opts does not verify under "
                           "empty context: %s",
                           error_text(err));
    mldsa_private_key_free(zero_sk);
    mldsa_public_key_free(zero_pk);
    arena_free(&ar);
}

/* ------------------------------------------------------------- Wycheproof */

static void TestVerifyWycheproof(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof gen_mldsa_verify / sizeof gen_mldsa_verify[0]; i++) {
        const GenMldsaVerify *tv = &gen_mldsa_verify[i];
        const GenMldsaVerifyGroup *g = &gen_mldsa_verify_groups[tv->group];
        Error err = BURROW_NO_ERROR;
        Slice pk_bytes = unhex_parts(a, g->pk);
        MldsaPublicKey *pub =
            mldsa_new_public_key(a, params_of(g->params), pk_bytes, &err);
        if (BURROW_FAILED(err)) {
            if (tv->pass)
                testing_t_fatalf_v(t, "%d/%d: NewPublicKey: %s", g->params, tv->tc_id,
                                   error_text(err));
            arena_reset(&ar);
            continue;
        }
        if (!bytes_equal(mldsa_public_key_bytes(pub, a), pk_bytes))
            testing_t_errorf_v(t, "%d/%d: public key roundtrip mismatch", g->params,
                               tv->tc_id);

        Slice msg = unhex_parts(a, tv->msg);
        Slice sig = unhex_parts(a, tv->sig);
        MldsaOptions opts = {BURROW_STR_EMPTY};
        if (tv->has_ctx)
            opts.context = str_of(unhex(a, tv->ctx));
        err = mldsa_verify(pub, msg, sig, &opts);
        if (tv->pass && BURROW_FAILED(err))
            testing_t_errorf_v(t, "%d/%d (%s): Verify: %s", g->params, tv->tc_id,
                               tv->comment, error_text(err));
        if (!tv->pass && !BURROW_FAILED(err))
            testing_t_errorf_v(t, "%d/%d (%s): Verify should have failed", g->params,
                               tv->tc_id, tv->comment);
        arena_reset(&ar);
    }
    arena_free(&ar);
}

/* runSignTest and runRandomizedSignTest of mldsa_wycheproof_test.go. The second
 * signs with the randomness of the vector, or 32 zero bytes when it has none,
 * through the functions only the tests reach. */
static void run_sign_test(TestingT *t, Alloc *a, const MldsaPrivateKey *priv,
                          const GenMldsaSign *tv, int params, bool randomized_api) {
    Slice msg = slice_nil(TYPE_BYTE), mu = slice_nil(TYPE_BYTE);
    MldsaOptions opts = {BURROW_STR_EMPTY};
    if (tv->has_msg) {
        msg = unhex_parts(a, tv->msg);
        if (tv->has_ctx)
            opts.context = str_of(unhex(a, tv->ctx));
    }
    bool has_mu = tv->mu[0] != 0;
    if (has_mu)
        mu = unhex(a, tv->mu);
    if (!tv->has_msg && !has_mu)
        testing_t_fatalf_v(t, "%d/%d: test vector has neither msg nor mu", params,
                           tv->tc_id);
    Byte zero_rnd[32] = {0};
    Slice rnd = tv->rnd[0] != 0 ? unhex(a, tv->rnd) : bs(zero_rnd, 32);

    Slice sig_msg = slice_nil(TYPE_BYTE), sig_mu = slice_nil(TYPE_BYTE);
    Error err_msg = BURROW_NO_ERROR, err_mu = BURROW_NO_ERROR;
    if (tv->has_msg) {
        if (randomized_api)
            sig_msg = burrow__mldsa_testing_only_sign_with_random(
                priv, a, msg, opts.context, rnd, &err_msg);
        else
            sig_msg = mldsa_private_key_sign_deterministic(
                priv, a, msg, mldsa_options_as_signer_opts(&opts), &err_msg);
    }
    if (has_mu) {
        if (randomized_api)
            sig_mu = burrow__mldsa_testing_only_sign_external_mu_with_random(
                priv, a, mu, rnd, &err_mu);
        else
            sig_mu = mldsa_private_key_sign_deterministic(
                priv, a, mu, crypto_hash_as_signer_opts(&mu_hash), &err_mu);
    }
    Error errs[2] = {err_msg, err_mu};
    for (int i = 0; i < 2; i++) {
        if (BURROW_FAILED(errs[i])) {
            if (tv->pass)
                testing_t_fatalf_v(t, "%d/%d (%s): Sign: %s", params, tv->tc_id,
                                   tv->comment, error_text(errs[i]));
            return;
        }
    }
    if (!tv->pass) {
        testing_t_errorf_v(t, "%d/%d (%s): Sign unexpectedly succeeded", params,
                           tv->tc_id, tv->comment);
        return;
    }

    Slice sig = tv->has_msg ? sig_msg : sig_mu;
    if (tv->has_msg && has_mu && !bytes_equal(sig_msg, sig_mu))
        testing_t_errorf_v(t, "%d/%d: Sign(msg, ctx) and SignExternalMu(mu) disagree",
                           params, tv->tc_id);
    if (!sha256_is(sig, tv->sig_sha256))
        testing_t_errorf_v(t, "%d/%d (%s): signature mismatch", params, tv->tc_id,
                           tv->comment);

    const MldsaPublicKey *pub = pub_of(priv);
    Error err;
    if (tv->has_msg && BURROW_FAILED(err = mldsa_verify(pub, msg, sig, &opts)))
        testing_t_errorf_v(t, "%d/%d: Verify of own signature failed: %s", params,
                           tv->tc_id, error_text(err));
    if (randomized_api && has_mu &&
        BURROW_FAILED(err = burrow__mldsa_verify_external_mu(pub, mu, sig)))
        testing_t_errorf_v(t, "%d/%d: VerifyExternalMu of own signature failed: %s",
                           params, tv->tc_id, error_text(err));
}

static void sign_seed_wycheproof(TestingT *t, bool randomized_api) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof gen_mldsa_sign / sizeof gen_mldsa_sign[0]; i++) {
        const GenMldsaSign *tv = &gen_mldsa_sign[i];
        const GenMldsaSeedGroup *g = &gen_mldsa_seed_groups[tv->group];
        Error err = BURROW_NO_ERROR;
        MldsaPrivateKey *priv =
            mldsa_new_private_key(a, params_of(g->params), unhex(a, g->seed), &err);
        if (BURROW_FAILED(err)) {
            if (tv->pass)
                testing_t_fatalf_v(t, "%d/%d: NewPrivateKey: %s", g->params, tv->tc_id,
                                   error_text(err));
            arena_reset(&ar);
            continue;
        }
        /* With the public key checked, the sign vectors double as key
         * generation vectors. */
        if (g->pk_sha256[0] != 0 &&
            !sha256_is(mldsa_public_key_bytes(pub_of(priv), a), g->pk_sha256))
            testing_t_fatalf_v(t, "%d/%d: public key mismatch", g->params, tv->tc_id);
        /* Randomized signatures cannot be made with the public API. */
        if (randomized_api || !tv->randomized)
            run_sign_test(t, a, priv, tv, g->params, randomized_api);
        arena_reset(&ar);
    }
    arena_free(&ar);
}

static void TestSignSeedWycheproof(TestingT *t) {
    sign_seed_wycheproof(t, false);
}

static void TestMLDSASignSeedRandomizedWycheproof(TestingT *t) {
    sign_seed_wycheproof(t, true);
}

static void TestMLDSANoSeedWycheproof(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof gen_mldsa_noseed / sizeof gen_mldsa_noseed[0]; i++) {
        const GenMldsaNoseed *tv = &gen_mldsa_noseed[i];
        const GenMldsaNoseedGroup *g = &gen_mldsa_noseed_groups[tv->group];
        Error err = BURROW_NO_ERROR;
        MldsaPrivateKey *priv =
            burrow__mldsa_testing_only_new_private_key_from_semi_expanded(
                a, unhex_parts(a, g->sk), &err);
        if (BURROW_FAILED(err)) {
            if (tv->pass)
                testing_t_fatalf_v(
                    t, "%d/%d: TestingOnlyNewPrivateKeyFromSemiExpanded: %s", g->params,
                    tv->tc_id, error_text(err));
            arena_reset(&ar);
            continue;
        }
        if (mldsa_public_key_parameters(pub_of(priv)) != params_of(g->params))
            testing_t_errorf_v(t, "%d/%d: parameters mismatch", g->params, tv->tc_id);
        if (g->pk_sha256[0] != 0 &&
            !sha256_is(mldsa_public_key_bytes(pub_of(priv), a), g->pk_sha256))
            testing_t_fatalf_v(t, "%d/%d: public key mismatch", g->params, tv->tc_id);
        arena_reset(&ar);
    }
    arena_free(&ar);
}

/* ------------------------------------------------------- known answer tests */

static void acvp_kat(void *env, TestingT *t) {
    const GenMldsaKat *kat = env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    const MldsaParameters *params = params_of(kat->params);
    MldsaPrivateKey *priv = mldsa_new_private_key(a, params, unhex(a, kat->seed), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "NewPrivateKey: %s", error_text(err));

    /* For the path coverage tests, check that every rejection path is hit. The
     * ct0 rejection is only reachable for ML-DSA-44. */
    bool path = strncmp(kat->name, "Path/", 5) == 0;
    Reached reached = {0};
    MldsaRejectionHook hook = {reached_fn, &reached};

    Slice pk = mldsa_public_key_bytes(pub_of(priv), a);
    Slice sk = burrow__mldsa_testing_only_private_key_semi_expanded_bytes(priv, a);
    if (!sha256_is(slices_concat(a, (Slice[]){pk, sk}, 2), kat->key_sha256))
        testing_t_errorf_v(t, "Key hash mismatch, want %s", kat->key_sha256);

    MldsaPublicKey *pub = mldsa_new_public_key(a, params, pk, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "NewPublicKey: %s", error_text(err));
    if (!mldsa_public_key_equal(pub, mldsa_private_key_public(priv)))
        testing_t_errorf_v(t, "Parsed public key not equal to original");
    if (!burrow__mldsa_public_key_identical(pub, pub_of(priv)))
        testing_t_errorf_v(t, "Parsed public key not identical to original");

    /* The table gives a Sign_internal input, which is part of the preimage of
     * μ. */
    Slice tr = sha3_sum_shake256(a, pk, 64);
    Sha3SHAKE *h = sha3_new_shake256(a);
    sha3_shake_write(h, tr, NULL);
    sha3_shake_write(h, unhex(a, kat->msg), NULL);
    Slice mu = slice_make(a, TYPE_BYTE, 64, 64);
    sha3_shake_read(h, mu, NULL);
    Slice sig = burrow__mldsa_sign_external_mu_deterministic(priv, a, mu,
                                                             path ? &hook : NULL, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "SignExternalMuDeterministic: %s", error_text(err));
    if (!sha256_is(sig, kat->sig_sha256))
        testing_t_errorf_v(t, "Signature hash mismatch, want %s", kat->sig_sha256);

    if (BURROW_FAILED(err = burrow__mldsa_verify_external_mu(pub_of(priv), mu, sig)))
        testing_t_errorf_v(t, "Verify: %s", error_text(err));
    Slice wrong = slice_make(a, TYPE_BYTE, 64, 64);
    if (!BURROW_FAILED(burrow__mldsa_verify_external_mu(pub_of(priv), wrong, sig)))
        testing_t_errorf_v(t, "Verify passed on wrong message");
    if (path)
        check_reached(t, &reached, kat->params == 44);
    arena_free(&ar);
}

static void TestACVPRejectionKATs(TestingT *t) {
    for (size_t i = 0; i < sizeof gen_mldsa_kats / sizeof gen_mldsa_kats[0]; i++)
        testing_t_run(
            t, str_from_cstr(gen_mldsa_kats[i].name),
            BURROW_FN(TestingTFunc, acvp_kat, (void *)(uintptr_t)&gen_mldsa_kats[i]));
}

static void TestCASTRejectionPaths(TestingT *t) {
    Reached reached = {0};
    MldsaRejectionHook hook = {reached_fn, &reached};
    Error err = burrow__mldsa_fips140_cast(&hook);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "fips140CAST: %s", error_text(err));
    check_reached(t, &reached, true);
}

/* ---------------------------------------------------------------- burrow's */

/* The self test passes, which Go only runs in FIPS mode. */
static void TestCAST(TestingT *t) {
    Error err = burrow__mldsa_fips140_cast(NULL);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "fips140CAST: %s", error_text(err));
}

static void TestErrors(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    Byte zeros[64] = {0};
    const MldsaParameters *p = mldsa_mldsa44();

    mldsa_generate_key(a, NULL, &err);
    want_error(t, "GenerateKey", err, "mldsa: invalid parameters");
    mldsa_new_private_key(a, p, bs(zeros, 31), &err);
    want_error(t, "NewPrivateKey", err, "mldsa: invalid seed length");
    mldsa_new_public_key(a, p, bs(zeros, 64), &err);
    want_error(t, "NewPublicKey", err, "mldsa: invalid public key length");

    err = BURROW_NO_ERROR;
    MldsaPrivateKey *sk = mldsa_new_private_key(a, p, bs(zeros, 32), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "NewPrivateKey: %s", error_text(err));
    Slice msg = bs(zeros, 8);
    mldsa_private_key_sign(sk, a, (IoReader){0}, msg,
                           crypto_hash_as_signer_opts(&sha256_hash), &err);
    want_error(t, "Sign(SHA256)", err, "mldsa: invalid SignerOpts");
    MldsaOptions long_ctx = {strings_repeat(a, BURROW_S("x"), 256)};
    mldsa_private_key_sign_deterministic(sk, a, msg,
                                         mldsa_options_as_signer_opts(&long_ctx), &err);
    want_error(t, "SignDeterministic(long context)", err, "mldsa: context too long");
    mldsa_private_key_sign(sk, a, (IoReader){0}, bs(zeros, 63),
                           crypto_hash_as_signer_opts(&mu_hash), &err);
    want_error(t, "Sign(MLDSAMu)", err, "mldsa: invalid message hash length");
    mldsa_private_key_sign(NULL, a, (IoReader){0}, msg, no_opts(), &err);
    want_error(t, "Sign(NULL)", err, "mldsa: zero private key");
    burrow__mldsa_testing_only_sign_with_random(sk, a, msg, BURROW_STR_EMPTY,
                                                bs(zeros, 31), &err);
    want_error(t, "TestingOnlySignWithRandom", err, "mldsa: invalid random length");
    burrow__mldsa_testing_only_sign_with_random(sk, a, msg, long_ctx.context,
                                                bs(zeros, 32), &err);
    want_error(t, "TestingOnlySignWithRandom", err, "mldsa: context too long");
    burrow__mldsa_testing_only_sign_external_mu_with_random(sk, a, bs(zeros, 64),
                                                            bs(zeros, 33), &err);
    want_error(t, "TestingOnlySignExternalMuWithRandom", err,
               "mldsa: invalid random length");
    burrow__mldsa_sign_external_mu_deterministic(sk, a, bs(zeros, 32), NULL, &err);
    want_error(t, "SignExternalMuDeterministic", err,
               "mldsa: invalid message hash length");

    err = BURROW_NO_ERROR;
    Slice sig = mldsa_private_key_sign_deterministic(sk, a, msg, no_opts(), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "SignDeterministic: %s", error_text(err));
    const MldsaPublicKey *pk = pub_of(sk);
    want_error(t, "Verify(short)",
               mldsa_verify(pk, msg, slice_sub(sig, 1, sig.len), NULL),
               "mldsa: invalid signature length");
    Slice bad = slices_clone(a, sig);
    ((Byte *)bad.p)[0] ^= 1;
    want_error(t, "Verify(tampered)", mldsa_verify(pk, msg, bad, NULL),
               "mldsa: invalid signature");
    /* The last byte of the hint is the count for the last row, and one that is
     * past the hints there are is a bad encoding. */
    bad = slices_clone(a, sig);
    ((Byte *)bad.p)[bad.len - 1] = 0xff;
    want_error(t, "Verify(bad hint)", mldsa_verify(pk, msg, bad, NULL),
               "mldsa: invalid signature encoding");
    want_error(t, "Verify(long context)", mldsa_verify(pk, msg, sig, &long_ctx),
               "mldsa: context too long");
    want_error(t, "Verify(NULL)", mldsa_verify(NULL, msg, sig, NULL),
               "mldsa: nil public key");
    MldsaPublicKey *zero_pk = burrow__mldsa_new_zero_public_key(a);
    want_error(t, "Verify(zero)", mldsa_verify(zero_pk, msg, sig, NULL),
               "mldsa: zero public key");
    want_error(t, "VerifyExternalMu(short)",
               burrow__mldsa_verify_external_mu(pk, bs(zeros, 32), sig),
               "mldsa: invalid message hash length");

    Slice semi = burrow__mldsa_testing_only_private_key_semi_expanded_bytes(sk, a);
    burrow__mldsa_testing_only_new_private_key_from_semi_expanded(
        a, slice_sub(semi, 0, semi.len - 1), &err);
    want_error(t, "TestingOnlyNewPrivateKeyFromSemiExpanded", err,
               "mldsa: invalid semi-expanded private key size");
    /* A coefficient of s1 of 7, which is out of range for η = 2. */
    bad = slices_clone(a, semi);
    ((Byte *)bad.p)[128] = 0xff;
    burrow__mldsa_testing_only_new_private_key_from_semi_expanded(a, bad, &err);
    want_error(t, "TestingOnlyNewPrivateKeyFromSemiExpanded", err,
               "mldsa: coefficient out of range");
    /* t0 starts after ρ, K, tr and the 4 + 4 polynomials of s1 and s2, 96 bytes
     * each, and every 13 bit value of it is in range. */
    bad = slices_clone(a, semi);
    ((Byte *)bad.p)[128 + 8 * 96] ^= 1;
    burrow__mldsa_testing_only_new_private_key_from_semi_expanded(a, bad, &err);
    want_error(t, "TestingOnlyNewPrivateKeyFromSemiExpanded", err,
               "mldsa: semi-expanded private key inconsistent with t0");
    bad = slices_clone(a, semi);
    ((Byte *)bad.p)[64] ^= 1;
    burrow__mldsa_testing_only_new_private_key_from_semi_expanded(a, bad, &err);
    want_error(t, "TestingOnlyNewPrivateKeyFromSemiExpanded", err,
               "mldsa: semi-expanded private key inconsistent with public key hash");
    arena_free(&ar);
}

/* A private key back from its semi-expanded form signs as the original does,
 * though its seed is a random one. */
static void semi_expanded(void *env, TestingT *t) {
    const MldsaParameters *params = env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    MldsaPrivateKey *sk = must_generate(t, params, a);
    Slice semi = burrow__mldsa_testing_only_private_key_semi_expanded_bytes(sk, a);
    MldsaPrivateKey *sk2 =
        burrow__mldsa_testing_only_new_private_key_from_semi_expanded(a, semi, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "TestingOnlyNewPrivateKeyFromSemiExpanded: %s",
                           error_text(err));
    if (!bytes_equal(burrow__mldsa_testing_only_private_key_semi_expanded_bytes(sk2, a),
                     semi))
        testing_t_errorf_v(t, "semi-expanded encoding does not round trip");
    if (!burrow__mldsa_public_key_identical(pub_of(sk), pub_of(sk2)))
        testing_t_errorf_v(t, "public keys differ");
    if (mldsa_private_key_equal(sk, BURROW_ANY(TYPE_MLDSA_PRIVATE_KEY, sk2)))
        testing_t_errorf_v(t, "key with a random seed is Equal to the original");
    Slice msg = slices_clone(a, cs("semi-expanded"));
    Slice s1 = mldsa_private_key_sign_deterministic(sk, a, msg, no_opts(), &err);
    Slice s2 = mldsa_private_key_sign_deterministic(sk2, a, msg, no_opts(), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "SignDeterministic: %s", error_text(err));
    if (!bytes_equal(s1, s2))
        testing_t_errorf_v(t, "signatures differ");
    arena_free(&ar);
}

static void TestSemiExpanded(TestingT *t) {
    test_all_parameters(t, semi_expanded);
    Arena ar;
    arena_init(&ar, NULL, 0);
    Slice b = burrow__mldsa_testing_only_private_key_semi_expanded_bytes(
        NULL, arena_allocator(&ar));
    if (b.p != NULL)
        testing_t_errorf_v(t, "semi-expanded bytes of a NULL key are not nil");
    arena_free(&ar);
}

static void TestSigner(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    MldsaPrivateKey *sk = must_generate(t, mldsa_mldsa65(), a);
    CryptoSigner s = mldsa_private_key_signer(sk);
    if (s.vt->self_type != TYPE_MLDSA_PRIVATE_KEY)
        testing_t_errorf_v(t, "Signer type is %s", s.vt->self_type->name);
    if (!mldsa_public_key_equal(pub_of(sk), crypto_signer_public(s)))
        testing_t_errorf_v(t, "Signer public key is not the key's");
    Slice msg = slices_clone(a, cs("signer"));
    MldsaOptions ctx = {BURROW_S("ctx")};
    Slice sig = crypto_signer_sign(s, a, (IoReader){0}, msg,
                                   mldsa_options_as_signer_opts(&ctx), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Sign: %s", error_text(err));
    if (BURROW_FAILED(err = mldsa_verify(pub_of(sk), msg, sig, &ctx)))
        testing_t_errorf_v(t, "Verify: %s", error_text(err));
    if (mldsa_options_hash_func(&ctx) != 0 || mldsa_options_hash_func(NULL) != 0)
        testing_t_errorf_v(t, "Options.HashFunc is not zero");
    CryptoSignerOpts o = mldsa_options_as_signer_opts(&ctx);
    if (o.vt->self_type != TYPE_MLDSA_OPTIONS || o.vt->hash_func(o.data) != 0)
        testing_t_errorf_v(t, "Options as SignerOpts is wrong");
    Slice seed = mldsa_private_key_bytes(NULL, a);
    if (seed.len != MLDSA_PRIVATE_KEY_SIZE)
        testing_t_errorf_v(t, "Bytes of a NULL key is %d bytes", seed.len);
    arena_free(&ar);
}

/* Keys from the heap, freed one by one, and the public key of a private key,
 * which its free leaves alone. */
static void TestHeapKeys(TestingT *t) {
    Error err = BURROW_NO_ERROR;
    MldsaPrivateKey *sk = mldsa_generate_key(NULL, mldsa_mldsa87(), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    mldsa_public_key_free((MldsaPublicKey *)(uintptr_t)pub_of(sk));
    Slice pk_bytes = mldsa_public_key_bytes(pub_of(sk), NULL);
    MldsaPublicKey *pk = mldsa_new_public_key(NULL, mldsa_mldsa87(), pk_bytes, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    Byte m[4] = {1, 2, 3, 4};
    Slice sig =
        mldsa_private_key_sign(sk, NULL, (IoReader){0}, bs(m, 4), no_opts(), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    if (BURROW_FAILED(err = mldsa_verify(pk, bs(m, 4), sig, NULL)))
        testing_t_errorf_v(t, "Verify with heap keys: %s", error_text(err));
    Slice seed = mldsa_private_key_bytes(sk, NULL);
    Slice owned[] = {pk_bytes, sig, seed};
    for (size_t i = 0; i < sizeof owned / sizeof owned[0]; i++)
        mem_free(heap_allocator(), owned[i].p, (size_t)owned[i].cap, 1);
    mldsa_public_key_free(pk);
    mldsa_private_key_free(sk);
    mldsa_public_key_free(NULL);
    mldsa_private_key_free(NULL);
}

/* ------------------------------------------------------------ benchmarks */

static volatile Byte sink;

typedef struct {
    int params;
    const char *const *messages;
    size_t n;
} SignBench;

/* Signing works by rejection sampling, which gives individual signing times a
 * massive variance. These messages are engineered to have the same
 * distribution of rejection counts and reasons as the average case. */
static void bench_sign(void *env, TestingB *b) {
    const SignBench *sb = env;
    Arena keys, ar;
    arena_init(&keys, NULL, 0);
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Byte seed[32] = {0};
    Error err = BURROW_NO_ERROR;
    MldsaPrivateKey *priv = mldsa_new_private_key(
        arena_allocator(&keys), params_of(sb->params), bs(seed, 32), &err);
    if (BURROW_FAILED(err))
        testing_b_fatalf_v(b, "NewPrivateKey: %s", error_text(err));
    size_t i = 0;
    while (testing_b_loop(b)) {
        Slice msg = cs(sb->messages[i]);
        if (++i >= sb->n)
            i = 0;
        Slice sig = mldsa_private_key_sign_deterministic(priv, a, msg, no_opts(), &err);
        sink ^= ((Byte *)sig.p)[0];
        arena_reset(&ar);
    }
    arena_free(&ar);
    arena_free(&keys);
}

static void BenchmarkSign(TestingB *b) {
    static const SignBench sets[] = {
        {44, gen_mldsa_bench44, sizeof gen_mldsa_bench44 / sizeof gen_mldsa_bench44[0]},
        {65, gen_mldsa_bench65, sizeof gen_mldsa_bench65 / sizeof gen_mldsa_bench65[0]},
        {87, gen_mldsa_bench87, sizeof gen_mldsa_bench87 / sizeof gen_mldsa_bench87[0]},
    };
    for (size_t i = 0; i < 3; i++)
        testing_b_run(b, mldsa_parameters_string(params_of(sets[i].params)),
                      BURROW_FN(TestingBFunc, bench_sign, (void *)(uintptr_t)&sets[i]));
}

typedef struct {
    const MldsaParameters *params;
    Slice pub, msg, sig;
    MldsaOptions opts;
} VerifyBench;

/* "Whole" parses the public key and verifies, since precomputation can be moved
 * between the two and most verifications are with fresh public keys. */
static void bench_verify_whole(void *env, TestingB *b) {
    const VerifyBench *vb = env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    while (testing_b_loop(b)) {
        Error err = BURROW_NO_ERROR;
        MldsaPublicKey *pk = mldsa_new_public_key(a, vb->params, vb->pub, &err);
        if (BURROW_FAILED(err))
            testing_b_fatalf_v(b, "NewPublicKey: %s", error_text(err));
        if (BURROW_FAILED(err = mldsa_verify(pk, vb->msg, vb->sig, &vb->opts)))
            testing_b_fatalf_v(b, "Verify: %s", error_text(err));
        arena_reset(&ar);
    }
    arena_free(&ar);
}

/* "Precomputed" runs only Verify with a parsed public key. */
static void bench_verify_precomputed(void *env, TestingB *b) {
    const VerifyBench *vb = env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Error err = BURROW_NO_ERROR;
    MldsaPublicKey *pk =
        mldsa_new_public_key(arena_allocator(&ar), vb->params, vb->pub, &err);
    if (BURROW_FAILED(err))
        testing_b_fatalf_v(b, "NewPublicKey: %s", error_text(err));
    while (testing_b_loop(b)) {
        if (BURROW_FAILED(err = mldsa_verify(pk, vb->msg, vb->sig, &vb->opts)))
            testing_b_fatalf_v(b, "Verify: %s", error_text(err));
    }
    arena_free(&ar);
}

static void bench_verify(void *env, TestingB *b) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    VerifyBench vb = {env,
                      slice_nil(TYPE_BYTE),
                      slice_nil(TYPE_BYTE),
                      slice_nil(TYPE_BYTE),
                      {BURROW_S("context")}};
    MldsaPrivateKey *priv = mldsa_generate_key(a, vb.params, &err);
    if (BURROW_FAILED(err))
        testing_b_fatalf_v(b, "GenerateKey: %s", error_text(err));
    vb.msg = slice_make(a, TYPE_BYTE, 128, 128);
    vb.sig = mldsa_private_key_sign_deterministic(
        priv, a, vb.msg, mldsa_options_as_signer_opts(&vb.opts), &err);
    if (BURROW_FAILED(err))
        testing_b_fatalf_v(b, "SignDeterministic: %s", error_text(err));
    vb.pub = mldsa_public_key_bytes(pub_of(priv), a);
    testing_b_run(b, BURROW_S("Whole"),
                  BURROW_FN(TestingBFunc, bench_verify_whole, &vb));
    testing_b_run(b, BURROW_S("Precomputed"),
                  BURROW_FN(TestingBFunc, bench_verify_precomputed, &vb));
    arena_free(&ar);
}

static void bench_all_parameters(TestingB *b, void (*f)(void *, TestingB *)) {
    const MldsaParameters *all[] = {mldsa_mldsa44(), mldsa_mldsa65(), mldsa_mldsa87()};
    for (size_t i = 0; i < 3; i++)
        testing_b_run(b, mldsa_parameters_string(all[i]),
                      BURROW_FN(TestingBFunc, f, (void *)(uintptr_t)all[i]));
}

static void BenchmarkVerify(TestingB *b) {
    bench_all_parameters(b, bench_verify);
}

static void bench_keygen(void *env, TestingB *b) {
    const MldsaParameters *params = env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Byte seed[32] = {0};
    while (testing_b_loop(b)) {
        MldsaPrivateKey *sk = mldsa_new_private_key(a, params, bs(seed, 32), NULL);
        sink ^= mldsa_private_key_bytes(sk, a).len != 0;
        arena_reset(&ar);
    }
    arena_free(&ar);
}

static void BenchmarkKeygen(TestingB *b) {
    bench_all_parameters(b, bench_keygen);
}

typedef struct {
    int params;
    const char *seed, *mu, *sk_hash, *sig_hash;
} CastBench;

/* Which is faster: the four rejections of ML-DSA-44, or the three of
 * ML-DSA-65. The first is TestACVPRejectionKATs/Path/ML-DSA-44/1 and the
 * second Path/ML-DSA-65/4, the only one that covers all three paths. */
static void bench_cast(void *env, TestingB *b) {
    const CastBench *c = env;
    Arena keep, ar;
    arena_init(&keep, NULL, 0);
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice seed = unhex(arena_allocator(&keep), c->seed);
    Slice mu = unhex(arena_allocator(&keep), c->mu);
    while (testing_b_loop(b)) {
        Error err = BURROW_NO_ERROR;
        MldsaPrivateKey *priv =
            mldsa_new_private_key(a, params_of(c->params), seed, &err);
        if (BURROW_FAILED(err))
            testing_b_fatalf_v(b, "NewPrivateKey: %s", error_text(err));
        Slice sk = burrow__mldsa_testing_only_private_key_semi_expanded_bytes(priv, a);
        if (!sha256_is(sk, c->sk_hash))
            testing_b_fatalf_v(b, "sk hash mismatch");
        Slice sig =
            burrow__mldsa_sign_external_mu_deterministic(priv, a, mu, NULL, &err);
        if (BURROW_FAILED(err))
            testing_b_fatalf_v(b, "SignExternalMuDeterministic: %s", error_text(err));
        if (!sha256_is(sig, c->sig_hash))
            testing_b_fatalf_v(b, "sig hash mismatch");
        if (BURROW_FAILED(err =
                              burrow__mldsa_verify_external_mu(pub_of(priv), mu, sig)))
            testing_b_fatalf_v(b, "Verify: %s", error_text(err));
        arena_reset(&ar);
    }
    arena_free(&ar);
    arena_free(&keep);
}

static void BenchmarkCAST(TestingB *b) {
    static const CastBench cases[] = {
        {44, "5C624FCC1862452452D0C665840D8237F43108E5499EDCDC108FBC49D596E4B7",
         "2ad1c72bb0fcbe28099ce8bd2ed836dfebe520aad38fbac66ef785a3cfb10fb4"
         "19327fa57818ee4e3718da4be48d24b59a208f8807271fdb7eda6e60141bd263",
         "29374951cb2bc3cda7315ce7f0ab99c7d2d65292e6c5156e8aa62ac14b1412af",
         "dcc71a421bc6ffafb7df0c7f6d018a19ada154d1e2ee360ed533cecd5dc980ad"},
        {65, "F215BA2280D86F142012FC05FFC04F2C7D22FF5DD7D69AA0EFB081E3A53E9318",
         "35cdb7dddbed44af4641bac659f46598ed769ea9693fd4ed2152b84c45811d2e"
         "66eded1eb20cde1c1f4b82642a330d8e86ac432a2aefaa56cd9b2b5f4affd450",
         "2e6f5ff659310b8ca1457a65d8b448b297a905dc08e06c1246a97daad0af6f7d",
         "c027d21b21fa75abe7f35cd84a54e2e83bd352140bc8c49eab2c45004e7268a7"},
    };
    for (size_t i = 0; i < 2; i++)
        testing_b_run(
            b, mldsa_parameters_string(params_of(cases[i].params)),
            BURROW_FN(TestingBFunc, bench_cast, (void *)(uintptr_t)&cases[i]));
}

#define TESTS(X)                                                                       \
    X(TestToFromMontgomery)                                                            \
    X(TestFieldAdd)                                                                    \
    X(TestFieldSub)                                                                    \
    X(TestFieldSubToMontgomery)                                                        \
    X(TestFieldReduceOnce)                                                             \
    X(TestFieldMul)                                                                    \
    X(TestFieldToMontgomeryOverflow)                                                   \
    X(TestFieldMulSub)                                                                 \
    X(TestFieldAddMul)                                                                 \
    X(TestCenteredMod)                                                                 \
    X(TestInfinityNorm)                                                                \
    X(TestConstants)                                                                   \
    X(TestPower2Round)                                                                 \
    X(TestDecompose)                                                                   \
    X(TestZetas)                                                                       \
    X(TestFieldAccumulated)                                                            \
    X(TestAccumulated)                                                                 \
    X(TestGenerateKey)                                                                 \
    X(TestParametersIdentity)                                                          \
    X(TestSign)                                                                        \
    X(TestExternalMu)                                                                  \
    X(TestPublicKey)                                                                   \
    X(TestEqualWrongType)                                                              \
    X(TestInvalidParameters)                                                           \
    X(TestInvalidSize)                                                                 \
    X(TestUninitialized)                                                               \
    X(TestVerifyWycheproof)                                                            \
    X(TestSignSeedWycheproof)                                                          \
    X(TestMLDSASignSeedRandomizedWycheproof)                                           \
    X(TestMLDSANoSeedWycheproof)                                                       \
    X(TestACVPRejectionKATs)                                                           \
    X(TestCASTRejectionPaths)                                                          \
    X(TestCAST)                                                                        \
    X(TestErrors)                                                                      \
    X(TestSemiExpanded)                                                                \
    X(TestSigner)                                                                      \
    X(TestHeapKeys)                                                                    \
    X(BenchmarkSign)                                                                   \
    X(BenchmarkVerify)                                                                 \
    X(BenchmarkKeygen)                                                                 \
    X(BenchmarkCAST)

TESTING_MAIN(TESTS)
