/* Derived from Go's src/crypto/internal/fips140test/nistec_test.go.
 * Go source: go1.27.1.
 *
 * TestEquivalents and TestScalarMult, for the four curves of nistec.h. Go
 * checks against crypto/elliptic for the order of each curve, and here it is
 * written out. TestNISTECAllocations has nothing to count, as nothing in
 * nistec.h allocates.
 *
 * TestCompressed is burrow's own. Go tests the compressed encoding through
 * crypto/elliptic's UnmarshalCompressed, which burrow does not have yet, and
 * the decoder is the one user of each curve's square root.
 *
 * Copyright 2021 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/bytes.h"
#include "burrow/encoding/hex.h"
#include "burrow/math/big.h"

#include "../src/crypto/nistec.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct Curve {
    const char *name;
    const NistecCurve *c;
    /* The order of the generator, in hex. */
    const char *n;
} Curve;

static const Curve curves[] = {
    {"P224", &burrow__nistec_p224,
     "ffffffffffffffffffffffffffff16a2e0b8f03e13dd29455c5c2a3d"},
    {"P256", &burrow__nistec_p256,
     "ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551"},
    {"P384", &burrow__nistec_p384,
     "ffffffffffffffffffffffffffffffffffffffffffffffffc7634d81f4372ddf581a0db248b0a77a"
     "ecec196accc52973"},
    {"P521", &burrow__nistec_p521,
     "01fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffa51868783"
     "bf2f966b7fcc0148f709a5d03bb5c9b8899c47aebb6fb71e91386409"},
};

static void run_curves(TestingT *t, void (*f)(void *env, TestingT *t)) {
    for (size_t i = 0; i < sizeof curves / sizeof curves[0]; i++)
        testing_t_run(t, str_from_cstr(curves[i].name),
                      BURROW_FN(TestingTFunc, f, (void *)(uintptr_t)&curves[i]));
}

static void order(BigInt *n, const Curve *c) {
    bool ok = false;
    big_int_set_string(n, str_from_cstr(c->n), 16, &ok);
    if (!ok)
        panic_str(BURROW_S("bad order"));
}

/* The bytes of p's uncompressed encoding, from a. */
static Slice point_bytes(Alloc *a, const NistecCurve *c, const NistecPoint *p) {
    uint8_t out[NISTEC_MAX_POINT_BYTES];
    Int n = c->bytes(p, out);
    Slice s = slice_make(a, TYPE_BYTE, n, n);
    memcpy(s.p, out, (size_t)n);
    return s;
}

static Slice infinity_bytes(Alloc *a, const NistecCurve *c) {
    NistecPoint p;
    return point_bytes(a, c, c->point_new(&p));
}

static Slice fill(Alloc *a, const BigInt *x, Int n) {
    return big_int_fill_bytes(x, slice_make(a, TYPE_BYTE, n, n));
}

static void fatal_if_err(TestingT *t, const void *p, Error err) {
    if (p == NULL || BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
}

/* ------------------------------------------------------------ Equivalents */

static void equivalents(void *env, TestingT *t) {
    const Curve *cv = env;
    const NistecCurve *c = cv->c;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;

    NistecPoint p;
    c->set_generator(&p);

    Int size = c->element_bytes;
    Slice two = slice_make(a, TYPE_BYTE, size, size);
    ((uint8_t *)two.p)[size - 1] = 2;
    BigInt n = BIG_INT(a);
    order(&n, cv);
    big_int_add(&n, &n, big_new_int(a, 2));
    Slice n_plus_two = fill(a, &n, size);

    NistecPoint p1, p2, p3, p4, p5, p6;
    c->double_(&p1, &p);
    c->add(&p2, &p, &p);
    fatal_if_err(t, c->scalar_mult(&p3, &p, two, &err), err);
    fatal_if_err(t, c->scalar_base_mult(&p4, two, &err), err);
    fatal_if_err(t, c->scalar_mult(&p5, &p, n_plus_two, &err), err);
    fatal_if_err(t, c->scalar_base_mult(&p6, n_plus_two, &err), err);

    Slice b1 = point_bytes(a, c, &p1);
    if (!bytes_equal(b1, point_bytes(a, c, &p2)))
        testing_t_errorf_v(t, "P+P != 2*P");
    if (!bytes_equal(b1, point_bytes(a, c, &p3)))
        testing_t_errorf_v(t, "P+P != [2]P");
    if (!bytes_equal(b1, point_bytes(a, c, &p4)))
        testing_t_errorf_v(t, "G+G != [2]G");
    if (!bytes_equal(b1, point_bytes(a, c, &p5)))
        testing_t_errorf_v(t, "P+P != [N+2]P");
    if (!bytes_equal(b1, point_bytes(a, c, &p6)))
        testing_t_errorf_v(t, "G+G != [N+2]G");

    arena_free(&ar);
}

static void TestEquivalents(TestingT *t) {
    run_curves(t, equivalents);
}

/* ------------------------------------------------------------- ScalarMult */

typedef struct ScalarEnv {
    const Curve *cv;
    Slice scalar;
} ScalarEnv;

static void check_scalar(void *env, TestingT *t) {
    const ScalarEnv *e = env;
    const NistecCurve *c = e->cv->c;
    Slice scalar = e->scalar;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;

    NistecPoint g, p1, p2;
    c->set_generator(&g);
    fatal_if_err(t, c->scalar_base_mult(&p1, scalar, &err), err);
    fatal_if_err(t, c->scalar_mult(&p2, &g, scalar, &err), err);
    Slice b1 = point_bytes(a, c, &p1);
    Slice b2 = point_bytes(a, c, &p2);
    if (!bytes_equal(b1, b2))
        testing_t_errorf_v(t, "[k]G != ScalarBaseMult(k)");

    BigInt n = BIG_INT(a), k = BIG_INT(a);
    order(&n, e->cv);
    big_int_set_bytes(&k, scalar);
    big_int_mod(&k, &k, &n);
    Slice inf = infinity_bytes(a, c);
    if (big_int_sign(&k) == 0) {
        if (!bytes_equal(b1, inf))
            testing_t_errorf_v(t, "ScalarBaseMult(k) != ∞");
        if (!bytes_equal(b2, inf))
            testing_t_errorf_v(t, "[k]G != ∞");
    } else {
        if (bytes_equal(b1, inf))
            testing_t_errorf_v(t, "ScalarBaseMult(k) == ∞");
        if (bytes_equal(b2, inf))
            testing_t_errorf_v(t, "[k]G == ∞");
    }

    BigInt d = BIG_INT(a);
    big_int_set_bytes(&d, scalar);
    big_int_sub(&d, &n, &d);
    big_int_mod(&d, &d, &n);
    NistecPoint g1;
    fatal_if_err(t, c->scalar_base_mult(&g1, fill(a, &d, scalar.len), &err), err);
    c->add(&g1, &g1, &p1);
    if (!bytes_equal(point_bytes(a, c, &g1), inf))
        testing_t_errorf_v(t, "[N - k]G + [k]G != ∞");

    arena_free(&ar);
}

static void check(TestingT *t, const Curve *cv, Str name, Slice scalar) {
    ScalarEnv e = {cv, scalar};
    testing_t_run(t, name, BURROW_FN(TestingTFunc, check_scalar, &e));
}

static void scalar_mult(void *env, TestingT *t) {
    const Curve *cv = env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    BigInt n = BIG_INT(a), s = BIG_INT(a);
    order(&n, cv);
    Int byte_len = big_int_bytes(&n, a).len;
    Int bit_len = big_int_bit_len(&n);

    check(t, cv, BURROW_S("0"), slice_make(a, TYPE_BYTE, byte_len, byte_len));
    check(t, cv, BURROW_S("1"), fill(a, big_new_int(a, 1), byte_len));
    check(t, cv, BURROW_S("N-1"),
          big_int_bytes(big_int_sub(&s, &n, big_new_int(a, 1)), a));
    check(t, cv, BURROW_S("N"), big_int_bytes(&n, a));
    check(t, cv, BURROW_S("N+1"),
          big_int_bytes(big_int_add(&s, &n, big_new_int(a, 1)), a));
    big_int_lsh(&s, big_new_int(a, 1), (Uint)bit_len);
    big_int_sub(&s, &s, big_new_int(a, 1));
    check(t, cv, BURROW_S("all1s"), big_int_bytes(&s, a));
    if (testing_short()) {
        arena_free(&ar);
        return;
    }
    char name[32];
    for (Int i = 0; i < bit_len; i++) {
        big_int_lsh(&s, big_new_int(a, 1), (Uint)i);
        snprintf(name, sizeof name, "1<<%d", (int)i);
        check(t, cv, str_from_cstr(name), fill(a, &s, byte_len));
    }
    for (int i = 0; i <= 64; i++) {
        snprintf(name, sizeof name, "%d", i);
        check(t, cv, str_from_cstr(name), fill(a, big_new_int(a, i), byte_len));
    }
    /* N-64 to N+64, since they risk overlapping with precomputed table values
     * in the final additions. */
    for (int i = -64; i <= 64; i++) {
        snprintf(name, sizeof name, "N%+d", i);
        big_int_add(&s, &n, big_new_int(a, i));
        check(t, cv, str_from_cstr(name), big_int_bytes(&s, a));
    }

    arena_free(&ar);
}

static void TestScalarMult(TestingT *t) {
    run_curves(t, scalar_mult);
}

/* ------------------------------------------------------------- Compressed */

static void compressed(void *env, TestingT *t) {
    const Curve *cv = env;
    const NistecCurve *c = cv->c;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;

    Int size = c->element_bytes;
    Slice k = slice_make(a, TYPE_BYTE, size, size);
    for (int i = 1; i <= 40; i++) {
        /* Small scalars and then ones with every byte set, which between them
         * give points with both signs of y. */
        for (Int j = 0; j < size; j++)
            ((uint8_t *)k.p)[j] = i <= 20 ? 0 : (uint8_t)(i * 37 + j);
        ((uint8_t *)k.p)[size - 1] = (uint8_t)i;
        if (c == &burrow__nistec_p521)
            ((uint8_t *)k.p)[0] &= 1;

        NistecPoint p, q;
        fatal_if_err(t, c->scalar_base_mult(&p, k, &err), err);
        uint8_t comp[NISTEC_MAX_POINT_BYTES];
        Int n = c->bytes_compressed(&p, comp);
        if (n != 1 + size)
            testing_t_fatalf_v(t, "compressed encoding is %d bytes", (int)n);
        c->point_new(&q);
        fatal_if_err(t, c->set_bytes(&q, slice_from(comp, n, n, TYPE_BYTE), &err), err);
        if (!bytes_equal(point_bytes(a, c, &p), point_bytes(a, c, &q)))
            testing_t_errorf_v(t,
                               "scalar %d: decoding the compressed encoding gives "
                               "another point",
                               i);
    }

    /* The point at infinity is one zero byte in either encoding. */
    NistecPoint inf, q;
    c->point_new(&inf);
    uint8_t comp[NISTEC_MAX_POINT_BYTES];
    if (c->bytes_compressed(&inf, comp) != 1 || comp[0] != 0)
        testing_t_errorf_v(t, "compressed infinity is not one zero byte");
    c->set_generator(&q);
    fatal_if_err(t, c->set_bytes(&q, slice_from(comp, 1, 1, TYPE_BYTE), &err), err);
    if (!bytes_equal(point_bytes(a, c, &q), infinity_bytes(a, c)))
        testing_t_errorf_v(t, "decoding infinity does not give infinity");

    /* x = 7 is on none of the four curves: x^3 - 3x + b is not a square
     * modulo any of the primes, and the decoder has to turn it away. */
    memset(comp, 0, sizeof comp);
    comp[0] = 2;
    comp[size] = 7;
    if (c->set_bytes(&q, slice_from(comp, 1 + size, 1 + size, TYPE_BYTE), &err) != NULL)
        testing_t_errorf_v(t, "a compressed point with no square root was accepted");

    arena_free(&ar);
}

static void TestCompressed(TestingT *t) {
    run_curves(t, compressed);
}

#define TESTS(X)                                                                       \
    X(TestEquivalents)                                                                 \
    X(TestScalarMult)                                                                  \
    X(TestCompressed)

TESTING_MAIN(TESTS)
