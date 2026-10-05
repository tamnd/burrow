/* Derived from Go's src/crypto/ecdh/ecdh_test.go.
 * Go source: go1.27.1.
 *
 * TestLinker builds a Go program and looks at its symbols, and has no C
 * counterpart: which curves end up in a binary is the linker's business here.
 * TestKeyExchanger and the cryptocustomrand=1 half of TestGenerateKey are
 * burrow's own.
 *
 * Copyright 2022 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/bytes.h"
#include "burrow/crypto.h"
#include "burrow/crypto/ecdh.h"
#include "burrow/crypto/rand.h"
#include "burrow/encoding/hex.h"
#include "burrow/mem/heap.h"
#include "burrow/strings.h"

#include "../src/crypto/rand_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static Slice unhex(Alloc *a, const char *s) {
    Error err = BURROW_NO_ERROR;
    Slice b = hex_decode_string(a, str_from_cstr(s), &err);
    if (BURROW_FAILED(err))
        panic_str(BURROW_S("invalid hex"));
    return b;
}

/* strings.Repeat(s, n) as a C string, from a. */
static const char *repeat(Alloc *a, const char *s, Int n) {
    Str r = strings_repeat(a, str_from_cstr(s), n);
    char *c = mem_alloc(a, (size_t)r.len + 1, 1);
    memcpy(c, r.p, (size_t)r.len);
    c[r.len] = 0;
    return c;
}

typedef struct NamedCurve {
    const char *name;
    const EcdhCurve *(*curve)(void);
} NamedCurve;

static const NamedCurve curves[] = {
    {"P256", ecdh_p256},
    {"P384", ecdh_p384},
    {"P521", ecdh_p521},
    {"X25519", ecdh_x25519},
};

#define NCURVES ((int)(sizeof curves / sizeof curves[0]))

static void test_all_curves(TestingT *t, void (*f)(void *env, TestingT *t)) {
    for (int i = 0; i < NCURVES; i++)
        testing_t_run(t, str_from_cstr(curves[i].name),
                      BURROW_FN(TestingTFunc, f, (void *)(uintptr_t)&curves[i]));
}

/* ------------------------------------------------------------------ ECDH */

static void ecdh_one(void *env, TestingT *t) {
    const EcdhCurve *curve = ((const NamedCurve *)env)->curve();
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;

    EcdhPrivateKey *alice_key =
        ecdh_curve_generate_key(curve, a, crypto_rand_reader, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    EcdhPrivateKey *bob_key =
        ecdh_curve_generate_key(curve, a, crypto_rand_reader, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));

    const EcdhPublicKey *alice_pub = ecdh_private_key_public_key(alice_key);
    EcdhPublicKey *alice_pub_key =
        ecdh_curve_new_public_key(curve, a, ecdh_public_key_bytes(alice_pub, a), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    if (!bytes_equal(ecdh_public_key_bytes(alice_pub, a),
                     ecdh_public_key_bytes(alice_pub_key, a)))
        testing_t_errorf_v(t, "encoded and decoded public keys are different");
    if (!ecdh_public_key_equal(alice_pub,
                               BURROW_ANY(TYPE_ECDH_PUBLIC_KEY, alice_pub_key)))
        testing_t_errorf_v(t, "encoded and decoded public keys are different");

    EcdhPrivateKey *alice_priv_key = ecdh_curve_new_private_key(
        curve, a, ecdh_private_key_bytes(alice_key, a), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    if (!bytes_equal(ecdh_private_key_bytes(alice_key, a),
                     ecdh_private_key_bytes(alice_priv_key, a)))
        testing_t_errorf_v(t, "encoded and decoded private keys are different");
    if (!ecdh_private_key_equal(alice_key,
                                BURROW_ANY(TYPE_ECDH_PRIVATE_KEY, alice_priv_key)))
        testing_t_errorf_v(t, "encoded and decoded private keys are different");

    Slice bob_secret = ecdh_private_key_ecdh(bob_key, a, alice_pub, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    Slice alice_secret =
        ecdh_private_key_ecdh(alice_key, a, ecdh_private_key_public_key(bob_key), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));

    if (!bytes_equal(bob_secret, alice_secret))
        testing_t_errorf_v(t, "two ECDH computations came out different");

    /* And the other way round from Equal: keys that differ. */
    if (ecdh_private_key_equal(alice_key, BURROW_ANY(TYPE_ECDH_PRIVATE_KEY, bob_key)))
        testing_t_errorf_v(t, "two generated private keys are equal");
    if (ecdh_public_key_equal(alice_pub, ecdh_private_key_public(bob_key)))
        testing_t_errorf_v(t, "two generated public keys are equal");
    if (!ecdh_public_key_equal(alice_pub, ecdh_private_key_public(alice_key)))
        testing_t_errorf_v(t, "PrivateKey.Public is not PrivateKey.PublicKey");

    arena_free(&ar);
}

static void TestECDH(TestingT *t) {
    test_all_curves(t, ecdh_one);
}

/* ----------------------------------------------------------- GenerateKey */

static const Type test_type = {
    {(const Byte *)"countingReader", 14},
    {(const Byte *)"ecdh_test", 9},
    KIND_STRUCT,
    0,
    1,
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x65636468U,
    NULL,
};

typedef struct CountingReader {
    IoReader r;
    Int n;
} CountingReader;

static Int counting_read(void *self, Slice p, Error *err) {
    CountingReader *r = self;
    Int n = r->r.vt->read(r->r.data, p, err);
    r->n += n;
    return n;
}

static const IoReaderVT counting_vt = {&test_type, counting_read};

static void generate_key_one(void *env, TestingT *t) {
    const EcdhCurve *curve = ((const NamedCurve *)env)->curve();
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;

    CountingReader cr = {crypto_rand_reader, 0};
    IoReader r = {&counting_vt, &cr};
    EcdhPrivateKey *k = ecdh_curve_generate_key(curve, a, r, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));

    /* GenerateKey does rejection sampling. If the masking works correctly,
     * the probability of a rejection is 1-ord(G)/2^ceil(log2(ord(G))), which
     * for all curves is small enough (at most 2^-32, for P-256) that a bit
     * flip is more likely to make this test fail than bad luck. Account for
     * the extra MaybeReadByte byte, too. */
    Int expected = ecdh_private_key_bytes(k, a).len + 1;
    if (cr.n > expected)
        testing_t_errorf_v(t,
                           "expected GenerateKey to consume at most %v bytes, got %v",
                           expected, cr.n);

    /* Without cryptocustomrand=1 the reader above is not read at all, so the
     * check means more with it. */
    burrow__crypto_rand_godebug_set("cryptocustomrand=1");
    cr.n = 0;
    k = ecdh_curve_generate_key(curve, a, r, &err);
    burrow__crypto_rand_godebug_set(NULL);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    if (cr.n < expected - 1 || cr.n > expected)
        testing_t_errorf_v(
            t,
            "with cryptocustomrand=1, GenerateKey read %v bytes, want %v "
            "or %v",
            cr.n, expected - 1, expected);

    /* nil is the system's generator. */
    k = ecdh_curve_generate_key(curve, a, (IoReader){NULL, NULL}, &err);
    if (BURROW_FAILED(err) || k == NULL)
        testing_t_fatalf_v(t, "GenerateKey(nil): %s", error_text(err));

    arena_free(&ar);
}

static void TestGenerateKey(TestingT *t) {
    test_all_curves(t, generate_key_one);
}

/* --------------------------------------------------------------- Vectors */

typedef struct Vector {
    const char *private_key, *public_key;
    const char *peer_public_key;
    const char *shared_secret;
} Vector;

static const Vector vectors[] = {
    /* NIST vectors from CAVS 14.1, ECC CDH Primitive (SP800-56A). */
    {
        "7d7dc5f71eb29ddaf80d6214632eeae03d9058af1fb6d22ed80badb62bc1a534",
        "04ead218590119e8876b29146ff89ca61770c4edbbf97d38ce385ed281d8a6b23028af61281f"
        "d35e2fa7002523acc85a429cb06ee6648325389f59edfce1405141",
        "04700c48f77f56584c5cc632ca65640db91b6bacce3a4df6b42ce7cc838833d287db71e509e3"
        "fd9b060ddb20ba5c51dcc5948d46fbf640dfe0441782cab85fa4ac",
        "46fc62106420ff012e54a434fbdd2d25ccc5852060561e68040dd7778997bd7b",
    },
    {
        "3cc3122a68f0d95027ad38c067916ba0eb8c38894d22e1b15618b6818a661774ad463b205da8"
        "8cf699ab4d43c9cf98a1",
        "049803807f2f6d2fd966cdd0290bd410c0190352fbec7ff6247de1302df86f25d34fe4a97bef"
        "60cff548355c015dbb3e5fba26ca69ec2f5b5d9dad20cc9da711383a9dbe34ea3fa5a2af75b4"
        "6502629ad54dd8b7d73a8abb06a3a3be47d650cc99",
        "04a7c76b970c3b5fe8b05d2838ae04ab47697b9eaf52e764592efda27fe7513272734466b400"
        "091adbf2d68c58e0c50066ac68f19f2e1cb879aed43a9969b91a0839c4c38a49749b661efedf"
        "243451915ed0905a32b060992b468c64766fc8437a",
        "5f9d29dc5e31a163060356213669c8ce132e22f57c9a04f40ba7fcead493b457e5621e766c40"
        "a2e3d4d6a04b25e533f1",
    },
    /* For some reason all field elements in the test vector (both scalars and
     * base field elements), but not the shared secret output, have two extra
     * leading zero bytes (which in big-endian are irrelevant). Removed here. */
    {
        "017eecc07ab4b329068fba65e56a1f8890aa935e57134ae0ffcce802735151f4eac6564f6ee9"
        "974c5e6887a1fefee5743ae2241bfeb95d5ce31ddcb6f9edb4d6fc47",
        "0400602f9d0cf9e526b29e22381c203c48a886c2b0673033366314f1ffbcba240ba42f4ef38a"
        "76174635f91e6b4ed34275eb01c8467d05ca80315bf1a7bbd945f550a501b7c85f26f5d4b2d7"
        "355cf6b02117659943762b6d1db5ab4f1dbc44ce7b2946eb6c7de342962893fd387d1b73d7a8"
        "672d1f236961170b7eb3579953ee5cdc88cd2d",
        "0400685a48e86c79f0f0875f7bc18d25eb5fc8c0b07e5da4f4370f3a9490340854334b1e1b87"
        "fa395464c60626124a4e70d0f785601d37c09870ebf176666877a2046d01ba52c56fc8776d9e"
        "8f5db4f0cc27636d0b741bbe05400697942e80b739884a83bde99e0f6716939e632bc8986fa1"
        "8dccd443a348b6c3e522497955a4f3c302f676",
        "005fc70477c3e63bc3954bd0df3ea0d1f41ee21746ed95fc5e1fdf90930d5e136672d72cc770"
        "742d1711c3c3a4c334a0ad9759436a4d3c5bf6e74b9578fac148c831",
    },
    /* X25519 test vector from RFC 7748, Section 6.1. */
    {
        "77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a",
        "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a",
        "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f",
        "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742",
    },
};

static void vectors_one(void *env, TestingT *t) {
    const NamedCurve *nc = env;
    const EcdhCurve *curve = nc->curve();
    const Vector *v = &vectors[nc - curves];
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;

    EcdhPrivateKey *key =
        ecdh_curve_new_private_key(curve, a, unhex(a, v->private_key), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    if (!bytes_equal(ecdh_public_key_bytes(ecdh_private_key_public_key(key), a),
                     unhex(a, v->public_key)))
        testing_t_errorf_v(t, "public key derived from the private key does not match");
    EcdhPublicKey *peer =
        ecdh_curve_new_public_key(curve, a, unhex(a, v->peer_public_key), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    Slice secret = ecdh_private_key_ecdh(key, a, peer, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    if (!bytes_equal(secret, unhex(a, v->shared_secret)))
        testing_t_errorf_v(t, "shared secret does not match: %x %s", secret,
                           v->shared_secret);

    arena_free(&ar);
}

static void TestVectors(TestingT *t) {
    test_all_curves(t, vectors_one);
}

/* ---------------------------------------------------------------- String */

static void string_one(void *env, TestingT *t) {
    static const char *want[] = {"P-256", "P-384", "P-521", "X25519"};
    const NamedCurve *nc = env;
    Str s = ecdh_curve_string(nc->curve());
    if (s.len == 0 || (s.p[0] != 'P' && s.p[0] != 'X'))
        testing_t_errorf_v(t, "unexpected Curve string encoding: %q", s);
    if (!str_eq(s, str_from_cstr(want[nc - curves])))
        testing_t_errorf_v(t, "Curve string is %q, want %q", s, want[nc - curves]);
}

static void TestString(TestingT *t) {
    test_all_curves(t, string_one);
}

/* --------------------------------------------------------- X25519Failure */

typedef struct FailureEnv {
    Slice private_key, public_key;
} FailureEnv;

static void x25519_failure(void *env, TestingT *t) {
    const FailureEnv *e = env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;

    EcdhPrivateKey *priv =
        ecdh_curve_new_private_key(ecdh_x25519(), a, e->private_key, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    EcdhPublicKey *pub =
        ecdh_curve_new_public_key(ecdh_x25519(), a, e->public_key, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    Slice secret = ecdh_private_key_ecdh(priv, a, pub, &err);
    if (!BURROW_FAILED(err))
        testing_t_errorf_v(t, "expected ECDH error");
    if (secret.p != NULL)
        testing_t_errorf_v(t, "unexpected ECDH output: %x", secret);

    arena_free(&ar);
}

static void TestX25519Failure(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;

    Slice identity =
        unhex(a, "0000000000000000000000000000000000000000000000000000000000000000");
    Slice low_order_point =
        unhex(a, "e0eb7a7c3b41b8ae1656e3faf19fc46ada098deb9c32b1fd866205165f49b800");
    Slice random_scalar = slice_make(a, TYPE_BYTE, 32, 32);
    crypto_rand_read(random_scalar, &err);

    FailureEnv e1 = {random_scalar, identity};
    testing_t_run(t, BURROW_S("identity point"),
                  BURROW_FN(TestingTFunc, x25519_failure, &e1));
    FailureEnv e2 = {random_scalar, low_order_point};
    testing_t_run(t, BURROW_S("low order point"),
                  BURROW_FN(TestingTFunc, x25519_failure, &e2));

    arena_free(&ar);
}

/* --------------------------------------------------------- NewPrivateKey */

/* NULL stands for strings.Repeat("01", 200), and "" ends a list. */
#define REPEAT01 NULL

static const char *const invalid_private_keys[][11] = {
    {
        /* Bad lengths. */
        "-",
        "01",
        "01010101010101010101010101010101010101010101010101010101010101",
        "000101010101010101010101010101010101010101010101010101010101010101",
        REPEAT01,
        /* Zero. */
        "0000000000000000000000000000000000000000000000000000000000000000",
        /* Order of the curve and above. */
        "ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551",
        "ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632552",
        "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
        "",
    },
    {
        /* Bad lengths. */
        "-",
        "01",
        "0101010101010101010101010101010101010101010101010101010101010101010101010101"
        "010101010101010101",
        "0001010101010101010101010101010101010101010101010101010101010101010101010101"
        "0101010101010101010101",
        REPEAT01,
        /* Zero. */
        "0000000000000000000000000000000000000000000000000000000000000000000000000000"
        "00000000000000000000",
        /* Order of the curve and above. */
        "ffffffffffffffffffffffffffffffffffffffffffffffffc7634d81f4372ddf581a0db248b0"
        "a77aecec196accc52973",
        "ffffffffffffffffffffffffffffffffffffffffffffffffc7634d81f4372ddf581a0db248b0"
        "a77aecec196accc52974",
        "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"
        "ffffffffffffffffffff",
        "",
    },
    {
        /* Bad lengths. */
        "-",
        "01",
        "0101010101010101010101010101010101010101010101010101010101010101010101010101"
        "010101010101010101010101010101010101010101010101010101",
        "0001010101010101010101010101010101010101010101010101010101010101010101010101"
        "0101010101010101010101010101010101010101010101010101010101",
        REPEAT01,
        /* Zero. */
        "0000000000000000000000000000000000000000000000000000000000000000000000000000"
        "00000000000000000000000000000000000000000000000000000000",
        /* Order of the curve and above. */
        "01fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffa51868783"
        "bf2f966b7fcc0148f709a5d03bb5c9b8899c47aebb6fb71e91386409",
        "01fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffa51868783"
        "bf2f966b7fcc0148f709a5d03bb5c9b8899c47aebb6fb71e9138640a",
        "11fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffa51868783"
        "bf2f966b7fcc0148f709a5d03bb5c9b8899c47aebb6fb71e91386409",
        "03fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff4a30d0f07"
        "7e5f2cd6ff980291ee134ba0776b937113388f5d76df6e3d2270c812",
        "",
    },
    {
        /* X25519 only rejects bad lengths. */
        "-",
        "01",
        "01010101010101010101010101010101010101010101010101010101010101",
        "000101010101010101010101010101010101010101010101010101010101010101",
        REPEAT01,
        "",
    },
};

/* An entry of the tables: "-" is the empty string, which "" can't be. */
static const char *entry(Alloc *a, const char *s) {
    if (s == REPEAT01)
        return repeat(a, "01", 200);
    if (strcmp(s, "-") == 0)
        return "";
    return s;
}

static void new_private_key_one(void *env, TestingT *t) {
    const NamedCurve *nc = env;
    const EcdhCurve *curve = nc->curve();
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    for (const char *const *in = invalid_private_keys[nc - curves];
         *in == REPEAT01 || **in != 0; in++) {
        const char *input = entry(a, *in);
        Error err = BURROW_NO_ERROR;
        EcdhPrivateKey *k = ecdh_curve_new_private_key(curve, a, unhex(a, input), &err);
        if (!BURROW_FAILED(err))
            testing_t_errorf_v(t, "unexpectedly accepted %q", input);
        else if (k != NULL)
            testing_t_errorf_v(t, "PrivateKey was not nil on error");
        else if (strings_contains(error_text(err), BURROW_S("boringcrypto")))
            testing_t_errorf_v(t, "boringcrypto error leaked out: %v", error_text(err));
    }

    arena_free(&ar);
}

static void TestNewPrivateKey(TestingT *t) {
    test_all_curves(t, new_private_key_one);
}

/* ---------------------------------------------------------- NewPublicKey */

static const char *const invalid_public_keys[][10] = {
    {
        /* Bad lengths. */
        "-",
        "04",
        REPEAT01,
        /* Infinity. */
        "00",
        /* Compressed encodings. */
        "036b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296",
        "02e2534a3532d08fbba02dde659ee62bd0031fe2db785596ef509302446b030852",
        /* Points not on the curve. */
        "046b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c2964fe342e2fe"
        "1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f6",
        "0400000000000000000000000000000000000000000000000000000000000000000000000000"
        "000000000000000000000000000000000000000000000000000000",
        /* Non-canonical encoding. */
        "04ffffffff00000001000000000000000000000001000000000000000000000004ba6dbc4555"
        "a7e7fa016ec431667e8521ee35afc49b265c3accbea3f7cdb70433",
        "",
    },
    {
        /* Bad lengths. */
        "-",
        "04",
        REPEAT01,
        /* Infinity. */
        "00",
        /* Compressed encodings. */
        "03aa87ca22be8b05378eb1c71ef320ad746e1d3b628ba79b9859f741e082542a385502f25dbf"
        "55296c3a545e3872760ab7",
        "0208d999057ba3d2d969260045c55b97f089025959a6f434d651d207d19fb96e9e4fe0e86ebe"
        "0e64f85b96a9c75295df61",
        /* Points not on the curve. */
        "04aa87ca22be8b05378eb1c71ef320ad746e1d3b628ba79b9859f741e082542a385502f25dbf"
        "55296c3a545e3872760ab73617de4a96262c6f5d9e98bf9292dc29f8f41dbd289a147ce9da31"
        "13b5f0b8c00a60b1ce1d7e819d7a431d7c90ea0e60",
        "0400000000000000000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000000000000000"
        "000000000000000000000000000000000000000000",
        /* Non-canonical encoding. */
        "04fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffeffffffff00"
        "0000000000000100000001732152442fb6ee5c3e6ce1d920c059bc623563814d79042b903ce6"
        "0f1d4487fccd450a86da03f3e6ed525d02017bfdb3",
        "",
    },
    {
        /* Bad lengths. */
        "-",
        "04",
        REPEAT01,
        /* Infinity. */
        "00",
        /* Compressed encodings. */
        "030035b5df64ae2ac204c354b483487c9070cdc61c891c5ff39afc06c5d55541d3ceac8659e2"
        "4afe3d0750e8b88e9f078af066a1d5025b08e5a5e2fbc87412871902f3",
        "0200c6858e06b70404e9cd9e3ecb662395b4429c648139053fb521f828af606b4d3dbaa14b5e"
        "77efe75928fe1dc127a2ffa8de3348b3c1856a429bf97e7e31c2e5bd66",
        /* Points not on the curve. */
        "0400c6858e06b70404e9cd9e3ecb662395b4429c648139053fb521f828af606b4d3dbaa14b5e"
        "77efe75928fe1dc127a2ffa8de3348b3c1856a429bf97e7e31c2e5bd66011839296a789a3bc0"
        "045c8a5fb42c7d1bd998f54449579b446817afbd17273e662c97ee72995ef42640c550b9013f"
        "ad0761353c7086a272c24088be94769fd16651",
        "0400000000000000000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000000000000000"
        "00000000000000000000000000000000000000",
        /* Non-canonical encoding. */
        "0402000000000000000000000000000000000000000000000000000000000000000000000000"
        "000000000000000000000000000000000000000000000000000000000100d9254fdf800496ac"
        "b33790b103c5ee9fac12832fe546c632225b0f7fce3da4574b1a879b623d722fa8fc34d5fc2a"
        "8731aad691a9a8bb8b554c95a051d6aa505acf",
        "",
    },
    {""},
};

static void new_public_key_one(void *env, TestingT *t) {
    const NamedCurve *nc = env;
    const EcdhCurve *curve = nc->curve();
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    for (const char *const *in = invalid_public_keys[nc - curves];
         *in == REPEAT01 || **in != 0; in++) {
        /* Go's tables have strings.Repeat("04", 200) here, where the private
         * key tables have "01". */
        const char *input = *in == REPEAT01 ? repeat(a, "04", 200) : entry(a, *in);
        Error err = BURROW_NO_ERROR;
        EcdhPublicKey *k = ecdh_curve_new_public_key(curve, a, unhex(a, input), &err);
        if (!BURROW_FAILED(err))
            testing_t_errorf_v(t, "unexpectedly accepted %q", input);
        else if (k != NULL)
            testing_t_errorf_v(t, "PublicKey was not nil on error");
        else if (strings_contains(error_text(err), BURROW_S("boringcrypto")))
            testing_t_errorf_v(t, "boringcrypto error leaked out: %v", error_text(err));
    }

    arena_free(&ar);
}

static void TestNewPublicKey(TestingT *t) {
    test_all_curves(t, new_public_key_one);
}

/* ---------------------------------------------------- MismatchedCurves */

typedef struct MismatchEnv {
    const EcdhPrivateKey *priv;
    const EcdhCurve *pub_curve;
} MismatchEnv;

static void mismatch_one(void *env, TestingT *t) {
    const MismatchEnv *e = env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;

    EcdhPrivateKey *pub =
        ecdh_curve_generate_key(e->pub_curve, a, crypto_rand_reader, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "failed to generate test key: %s", error_text(err));
    Str expected =
        BURROW_S("crypto/ecdh: private key and public key curves do not match");
    ecdh_private_key_ecdh(e->priv, a, ecdh_private_key_public_key(pub), &err);
    if (!BURROW_FAILED(err) || !str_eq(error_text(err), expected))
        testing_t_fatalf_v(t, "unexpected error: want %q, got %q", expected,
                           error_text(err));

    arena_free(&ar);
}

static void TestMismatchedCurves(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    for (int i = 0; i < NCURVES; i++) {
        Error err = BURROW_NO_ERROR;
        EcdhPrivateKey *priv =
            ecdh_curve_generate_key(curves[i].curve(), a, crypto_rand_reader, &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "failed to generate test key: %s", error_text(err));

        for (int j = 0; j < NCURVES; j++) {
            if (i == j)
                continue;
            char name[32];
            snprintf(name, sizeof name, "%s/%s", curves[i].name, curves[j].name);
            MismatchEnv e = {priv, curves[j].curve()};
            testing_t_run(t, str_from_cstr(name),
                          BURROW_FN(TestingTFunc, mismatch_one, &e));
        }
    }

    arena_free(&ar);
}

/* ---------------------------------------------------------- KeyExchanger */

static void key_exchanger_one(void *env, TestingT *t) {
    const EcdhCurve *curve = ((const NamedCurve *)env)->curve();
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;

    EcdhPrivateKey *k = ecdh_curve_generate_key(curve, a, crypto_rand_reader, &err);
    EcdhPrivateKey *peer = ecdh_curve_generate_key(curve, a, crypto_rand_reader, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));

    EcdhKeyExchanger x = ecdh_private_key_key_exchanger(k);
    if (ecdh_key_exchanger_public_key(x) != ecdh_private_key_public_key(k))
        testing_t_errorf_v(t, "KeyExchanger.PublicKey is not the key's");
    if (ecdh_key_exchanger_curve(x) != curve || ecdh_private_key_curve(k) != curve ||
        ecdh_public_key_curve(ecdh_private_key_public_key(k)) != curve)
        testing_t_errorf_v(t, "KeyExchanger.Curve is not the key's");
    Slice s1 = ecdh_key_exchanger_ecdh(x, a, ecdh_private_key_public_key(peer), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    Slice s2 = ecdh_private_key_ecdh(peer, a, ecdh_private_key_public_key(k), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    if (!bytes_equal(s1, s2))
        testing_t_errorf_v(t, "KeyExchanger.ECDH gave another secret");

    arena_free(&ar);
}

static void TestKeyExchanger(TestingT *t) {
    test_all_curves(t, key_exchanger_one);
}

/* The keys outside an arena, to check they are one block each and free
 * cleanly, which the sanitizer builds look at. */
static void TestHeapKeys(TestingT *t) {
    Alloc *a = heap_allocator();
    for (int i = 0; i < NCURVES; i++) {
        Error err = BURROW_NO_ERROR;
        EcdhPrivateKey *k =
            ecdh_curve_generate_key(curves[i].curve(), a, crypto_rand_reader, &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "%s", error_text(err));
        Slice b = ecdh_public_key_bytes(ecdh_private_key_public_key(k), a);
        EcdhPublicKey *p = ecdh_curve_new_public_key(curves[i].curve(), a, b, &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "%s", error_text(err));
        Slice s = ecdh_private_key_ecdh(k, a, p, &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "%s", error_text(err));
        mem_free(a, s.p, (size_t)s.cap, 1);
        mem_free(a, b.p, (size_t)b.cap, 1);
        ecdh_public_key_free(p);
        ecdh_private_key_free(k);
    }
    ecdh_private_key_free(NULL);
    ecdh_public_key_free(NULL);
}

#define TESTS(X)                                                                       \
    X(TestECDH)                                                                        \
    X(TestGenerateKey)                                                                 \
    X(TestVectors)                                                                     \
    X(TestString)                                                                      \
    X(TestX25519Failure)                                                               \
    X(TestNewPrivateKey)                                                               \
    X(TestNewPublicKey)                                                                \
    X(TestMismatchedCurves)                                                            \
    X(TestKeyExchanger)                                                                \
    X(TestHeapKeys)

TESTING_MAIN(TESTS)
