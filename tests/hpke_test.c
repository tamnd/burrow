/* Derived from Go's src/crypto/hpke/hpke_test.go.
 * Go source: go1.27.1.
 *
 * The vectors of testdata/rfc9180.json and testdata/hpke-pq.json are in
 * hpke_test_gen.h, which tools/gen-hpke-tests.sh writes. Go makes an
 * encapsulation come out the way a vector says by setting two package
 * variables for the length of the test. Here the same randomness goes to
 * burrow__hpke_new_sender_with as an argument.
 *
 * TestErrors and TestHeapKeys after Go's tests are burrow's, for the errors
 * Go's tests do not reach and for keys that live on the heap.
 *
 * Copyright 2024 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/crypto.h"
#include "burrow/crypto/ecdh.h"
#include "burrow/crypto/hpke.h"
#include "burrow/crypto/mlkem.h"
#include "burrow/crypto/mlkem/mlkemtest.h"
#include "burrow/crypto/sha3.h"
#include "burrow/encoding/hex.h"
#include "burrow/mem/heap.h"

#include "../src/crypto/hpke_internal.h"

#include "hpke_test_gen.h"

#include <stdint.h>
#include <string.h>

static Slice bs(void *p, Int n) {
    return slice_from(p, n, n, TYPE_BYTE);
}

/* A string as bytes, which nothing here writes to. */
static Slice sb(const char *s) {
    Int n = (Int)strlen(s);
    return slice_from((void *)(uintptr_t)s, n, n, TYPE_BYTE);
}

static Slice unhex(Alloc *a, const char *s) {
    Error err = BURROW_NO_ERROR;
    Slice b = hex_decode_string(a, str_from_cstr(s), &err);
    if (BURROW_FAILED(err))
        panic_str(BURROW_S("invalid hex"));
    return b;
}

static Str hexs(Alloc *a, Slice b) {
    return hex_encode_to_string(a, b);
}

static bool bytes_eq(Slice x, Slice y) {
    return x.len == y.len && (x.len == 0 || memcmp(x.p, y.p, (size_t)x.len) == 0);
}

/* The KEMs, KDFs and AEADs of TestRoundTrip. */
static size_t all_kems(const HpkeKEM *out[9]) {
    out[0] = hpke_dhkem(ecdh_p256());
    out[1] = hpke_dhkem(ecdh_p384());
    out[2] = hpke_dhkem(ecdh_p521());
    out[3] = hpke_dhkem(ecdh_x25519());
    out[4] = hpke_mlkem768();
    out[5] = hpke_mlkem1024();
    out[6] = hpke_mlkem768_p256();
    out[7] = hpke_mlkem1024_p384();
    out[8] = hpke_mlkem768_x25519();
    return 9;
}

static size_t all_kdfs(const HpkeKDF *out[5]) {
    out[0] = hpke_hkdfsha256();
    out[1] = hpke_hkdfsha384();
    out[2] = hpke_hkdfsha512();
    out[3] = hpke_shake128();
    out[4] = hpke_shake256();
    return 5;
}

static size_t all_aeads(const HpkeAEAD *out[3]) {
    out[0] = hpke_aes128_gcm();
    out[1] = hpke_aes256_gcm();
    out[2] = hpke_cha_cha20_poly1305();
    return 3;
}

/* ---------------------------------------------------------------- Example */

/* Go's Example: MLKEM768-X25519, HKDF-SHA256 and AES-256-GCM with the one-shot
 * API. */
static void TestExample(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    const HpkeKEM *kem = hpke_mlkem768_x25519();
    const HpkeKDF *kdf = hpke_hkdfsha256();
    const HpkeAEAD *aead = hpke_aes256_gcm();

    /* Recipient side */
    HpkePrivateKey *recipient_private_key = hpke_kem_generate_key(kem, a, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    Slice public_key_bytes =
        hpke_public_key_bytes(hpke_private_key_public_key(recipient_private_key), a);

    /* Sender side */
    HpkePublicKey *public_key = hpke_kem_new_public_key(kem, a, public_key_bytes, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    Slice ciphertext =
        hpke_seal(a, public_key, kdf, aead, sb("example"), sb("|-()-|"), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));

    /* Recipient side */
    Slice plaintext =
        hpke_open(a, recipient_private_key, kdf, aead, sb("example"), ciphertext, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    Str got = fmt_sprintf_v(a, "Decrypted message: %s\n",
                            str_from_bytes(plaintext.p, plaintext.len));
    if (!str_eq(got, BURROW_S("Decrypted message: |-()-|\n")))
        testing_t_errorf_v(t, "got %q", got);
    hpke_public_key_free(public_key);
    hpke_private_key_free(recipient_private_key);
    arena_free(&ar);
}

/* -------------------------------------------------------------- RoundTrip */

/* b in a slice with 2000 bytes of 0xAA past its end, which check_slice then
 * looks at. */
static Slice pad_slice(Alloc *a, Slice b) {
    Slice s = slice_make(a, TYPE_BYTE, b.len, b.len + 2000);
    memset(s.p, 0xAA, (size_t)s.cap);
    if (b.len > 0)
        memcpy(s.p, b.p, (size_t)b.len);
    return s;
}

static void check_slice(TestingT *t, const char *name, Slice s) {
    const Byte *p = s.p;
    for (Int i = s.len; i < s.cap; i++) {
        if (p[i] != 0xAA) {
            testing_t_errorf_v(t, "%s: modified byte at index %d beyond slice length",
                               str_from_cstr(name), i);
            return;
        }
    }
}

static void round_trip_suite(TestingT *t, Alloc *a, HpkePrivateKey *k,
                             HpkePrivateKey *kk, const HpkePublicKey *pk,
                             const HpkeKDF *kdf, const HpkeAEAD *aead) {
    Error err = BURROW_NO_ERROR;
    Slice c = hpke_seal(a, pk, kdf, aead, sb("info"), sb("plaintext"), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    Slice p = hpke_open(a, kk, kdf, aead, sb("info"), c, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    if (!bytes_eq(p, sb("plaintext")))
        testing_t_errorf_v(t, "unexpected plaintext: got %s, want %s", hexs(a, p),
                           hexs(a, sb("plaintext")));

    p = hpke_open(a, kk, kdf, aead, sb("wrong"), c, &err);
    if (BURROW_OK(err))
        testing_t_errorf_v(
            t, "expected error when opening with wrong info, got plaintext %s",
            hexs(a, p));
    err = BURROW_NO_ERROR;
    ((Byte *)c.p)[c.len - 1] ^= 0xFF;
    p = hpke_open(a, kk, kdf, aead, sb("info"), c, &err);
    if (BURROW_OK(err))
        testing_t_errorf_v(t,
                           "expected error when opening with corrupted ciphertext, got "
                           "plaintext %s",
                           hexs(a, p));
    err = BURROW_NO_ERROR;

    c = hpke_seal(a, hpke_private_key_public_key(k), kdf, aead, slice_nil(TYPE_BYTE),
                  slice_nil(TYPE_BYTE), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    p = hpke_open(a, k, kdf, aead, slice_nil(TYPE_BYTE), c, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    if (p.len != 0)
        testing_t_errorf_v(t, "unexpected plaintext: got %s, want empty", hexs(a, p));

    /* Test that Seal and Open don't modify the excess capacity of input
     * slices. This is a regression test for a bug where decap would append to
     * the enc slice, corrupting the ciphertext if they shared a backing
     * array. */
    Slice info_s = pad_slice(a, sb("info"));
    Slice plaintext_s = pad_slice(a, sb("plaintext"));
    c = hpke_seal(a, pk, kdf, aead, info_s, plaintext_s, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    check_slice(t, "Seal info", info_s);
    check_slice(t, "Seal plaintext", plaintext_s);

    Slice info_o = pad_slice(a, sb("info"));
    Slice ciphertext_o = pad_slice(a, c);
    p = hpke_open(a, kk, kdf, aead, info_o, ciphertext_o, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Open with large capacity slices failed: %s",
                           error_text(err));
    if (!bytes_eq(p, sb("plaintext")))
        testing_t_errorf_v(t, "unexpected plaintext: got %s, want %s", hexs(a, p),
                           hexs(a, sb("plaintext")));
    check_slice(t, "Open info", info_o);
    check_slice(t, "Open ciphertext", ciphertext_o);

    /* Also test the Sender.Seal and Recipient.Open methods. */
    Slice info_sender = pad_slice(a, sb("info"));
    HpkeSender *sender;
    Slice enc = hpke_new_sender(a, pk, kdf, aead, info_sender, &sender, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    check_slice(t, "NewSender info", info_sender);

    Slice aad_seal = pad_slice(a, sb("aad"));
    Slice plaintext_seal = pad_slice(a, sb("plaintext"));
    Slice ct = hpke_sender_seal(sender, a, aad_seal, plaintext_seal, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    check_slice(t, "Sender.Seal aad", aad_seal);
    check_slice(t, "Sender.Seal plaintext", plaintext_seal);

    Slice info_recipient = pad_slice(a, sb("info"));
    Slice enc_padded = pad_slice(a, enc);
    HpkeRecipient *recipient =
        hpke_new_recipient(a, enc_padded, kk, kdf, aead, info_recipient, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    check_slice(t, "NewRecipient info", info_recipient);
    check_slice(t, "NewRecipient enc", enc_padded);

    Slice aad_open = pad_slice(a, sb("aad"));
    Slice ct_padded = pad_slice(a, ct);
    p = hpke_recipient_open(recipient, a, aad_open, ct_padded, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Recipient.Open failed: %s", error_text(err));
    if (!bytes_eq(p, sb("plaintext")))
        testing_t_errorf_v(t, "unexpected plaintext: got %s, want %s", hexs(a, p),
                           hexs(a, sb("plaintext")));
    check_slice(t, "Recipient.Open aad", aad_open);
    check_slice(t, "Recipient.Open ciphertext", ct_padded);
}

typedef struct RoundTripEnv {
    const HpkeKEM *kem;
    const HpkeKDF *kdf;
    const HpkeAEAD *aead;
    HpkePrivateKey *k, *kk;
    const HpkePublicKey *pk;
    Alloc *a;
} RoundTripEnv;

static void round_trip_aead(void *env, TestingT *t) {
    const RoundTripEnv *e = env;
    round_trip_suite(t, e->a, e->k, e->kk, e->pk, e->kdf, e->aead);
}

static void round_trip_kdf(void *env, TestingT *t) {
    RoundTripEnv *e = env;
    const HpkeAEAD *aeads[3];
    size_t n = all_aeads(aeads);
    for (size_t i = 0; i < n; i++) {
        e->aead = aeads[i];
        testing_t_run(t, fmt_sprintf_v(e->a, "AEAD_%04x", (int)hpke_aead_id(aeads[i])),
                      BURROW_FN(TestingTFunc, round_trip_aead, e));
    }
}

static void round_trip_kem(void *env, TestingT *t) {
    const HpkeKEM *kem = env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    HpkePrivateKey *k = hpke_kem_generate_key(kem, a, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    Slice kb = hpke_private_key_bytes(k, a, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    HpkePrivateKey *kk = hpke_kem_new_private_key(kem, a, kb, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    Slice got = hpke_private_key_bytes(kk, a, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    if (!bytes_eq(got, kb))
        testing_t_errorf_v(t, "re-serialized key mismatch: got %s, want %s",
                           hexs(a, got), hexs(a, kb));
    Slice pkb = hpke_public_key_bytes(hpke_private_key_public_key(k), a);
    HpkePublicKey *pk = hpke_kem_new_public_key(kem, a, pkb, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    got = hpke_public_key_bytes(pk, a);
    if (!bytes_eq(got, pkb))
        testing_t_errorf_v(t, "re-serialized public key mismatch: got %s, want %s",
                           hexs(a, got), hexs(a, pkb));

    RoundTripEnv e = {kem, NULL, NULL, k, kk, pk, a};
    const HpkeKDF *kdfs[5];
    size_t n = all_kdfs(kdfs);
    for (size_t i = 0; i < n; i++) {
        e.kdf = kdfs[i];
        testing_t_run(t, fmt_sprintf_v(a, "KDF_%04x", (int)hpke_kdf_id(kdfs[i])),
                      BURROW_FN(TestingTFunc, round_trip_kdf, &e));
    }
    hpke_public_key_free(pk);
    hpke_private_key_free(kk);
    hpke_private_key_free(k);
    arena_free(&ar);
}

static void TestRoundTrip(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    const HpkeKEM *kems[9];
    size_t n = all_kems(kems);
    for (size_t i = 0; i < n; i++)
        testing_t_run(
            t,
            fmt_sprintf_v(arena_allocator(&ar), "KEM_%04x", (int)hpke_kem_id(kems[i])),
            BURROW_FN(TestingTFunc, round_trip_kem, (void *)(uintptr_t)kems[i]));
    arena_free(&ar);
}

/* ---------------------------------------------------------------- Vectors */

/* drawRandomInput: a length byte from r, then that many bytes. */
static Slice draw_random_input(Alloc *a, Sha3SHAKE *r) {
    Byte l;
    sha3_shake_read(r, bs(&l, 1), NULL);
    Slice b = slice_make(a, TYPE_BYTE, l, l);
    sha3_shake_read(r, b, NULL);
    return b;
}

/* The curve of a DHKEM or of the curve half of a hybrid. */
static const EcdhCurve *kem_curve(const HpkeKEM *kem) {
    if (kem == hpke_dhkem(ecdh_p256()) || kem == hpke_mlkem768_p256())
        return ecdh_p256();
    if (kem == hpke_dhkem(ecdh_p384()) || kem == hpke_mlkem1024_p384())
        return ecdh_p384();
    if (kem == hpke_dhkem(ecdh_p521()))
        return ecdh_p521();
    if (kem == hpke_dhkem(ecdh_x25519()) || kem == hpke_mlkem768_x25519())
        return ecdh_x25519();
    return NULL;
}

/* mlkemtest.Encapsulate768 or Encapsulate1024 with ek_bytes as the key, into
 * r. */
static bool fixed_pq(TestingT *t, Alloc *a, int pq, Slice ek_bytes, Slice rand,
                     HpkeEncapRandomness *r) {
    Error err = BURROW_NO_ERROR;
    if (pq == 768) {
        MlkemEncapsulationKey768 *ek =
            mlkem_new_encapsulation_key768(a, ek_bytes, &err);
        if (ek != NULL)
            r->pq_shared_key =
                mlkemtest_encapsulate768(ek, a, rand, &r->pq_ciphertext, &err);
        mlkem_encapsulation_key768_free(ek);
    } else {
        MlkemEncapsulationKey1024 *ek =
            mlkem_new_encapsulation_key1024(a, ek_bytes, &err);
        if (ek != NULL)
            r->pq_shared_key =
                mlkemtest_encapsulate1024(ek, a, rand, &r->pq_ciphertext, &err);
        mlkem_encapsulation_key1024_free(ek);
    }
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "%s", error_text(err));
        return false;
    }
    r->fixed_pq = true;
    return true;
}

/* setupDerandomizedEncap: the randomness that makes the encapsulation to pk
 * the one the vector has, from rand_bytes, its ikmE. *eph is the ephemeral key
 * to free afterwards. */
static bool setup_derandomized_encap(TestingT *t, Alloc *a, Slice rand_bytes,
                                     const HpkePublicKey *pk, HpkeEncapRandomness *r,
                                     EcdhPrivateKey **eph) {
    const HpkeKEM *kem = hpke_public_key_kem(pk);
    Slice pk_bytes = hpke_public_key_bytes(pk, a);
    Error err = BURROW_NO_ERROR;
    memset(r, 0, sizeof *r);
    *eph = NULL;
    const EcdhCurve *curve = kem_curve(kem);
    if (kem == hpke_mlkem768())
        return fixed_pq(t, a, 768, pk_bytes, rand_bytes, r);
    if (kem == hpke_mlkem1024())
        return fixed_pq(t, a, 1024, pk_bytes, rand_bytes, r);
    if (kem == hpke_mlkem768_x25519() || kem == hpke_mlkem768_p256() ||
        kem == hpke_mlkem1024_p384()) {
        int pq = kem == hpke_mlkem1024_p384() ? 1024 : 768;
        Int ek_size = pq == 768 ? MLKEM_ENCAPSULATION_KEY_SIZE768
                                : MLKEM_ENCAPSULATION_KEY_SIZE1024;
        Slice pq_rand = slice_sub(rand_bytes, 0, 32);
        /* For P-256 the rest of randBytes are the following candidates for
         * rejection sampling, but they are never reached. */
        Slice t_rand = kem == hpke_mlkem768_p256()
                           ? slice_sub(rand_bytes, 32, 64)
                           : slice_sub(rand_bytes, 32, rand_bytes.len);
        *eph = ecdh_curve_new_private_key(curve, a, t_rand, &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "%s", error_text(err));
            return false;
        }
        r->ephemeral = *eph;
        return fixed_pq(t, a, pq, slice_sub(pk_bytes, 0, ek_size), pq_rand, r);
    }
    if (curve == NULL) {
        testing_t_errorf_v(t, "unsupported KEM %04x", (int)hpke_kem_id(kem));
        return false;
    }
    /* Go hands over the ecdh.PrivateKey inside the key DeriveKeyPair gives.
     * Its bytes make the same key, clamped or not for X25519. */
    HpkePrivateKey *k = hpke_kem_derive_key_pair(kem, a, rand_bytes, &err);
    Slice kb = slice_nil(TYPE_BYTE);
    if (BURROW_OK(err))
        kb = hpke_private_key_bytes(k, a, &err);
    hpke_private_key_free(k);
    if (BURROW_OK(err))
        *eph = ecdh_curve_new_private_key(curve, a, kb, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "%s", error_text(err));
        return false;
    }
    r->ephemeral = *eph;
    return true;
}

static void check_bytes(TestingT *t, Alloc *a, const char *what, Slice got,
                        Slice want) {
    if (!bytes_eq(got, want))
        testing_t_errorf_v(t, "unexpected %s, got: %s, want %s", str_from_cstr(what),
                           hexs(a, got), hexs(a, want));
}

static void test_vector(void *env, TestingT *t) {
    const GenHpkeVector *v = env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;

    const HpkeKDF *kdf = hpke_new_kdf(v->kdf, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    if (hpke_kdf_id(kdf) != v->kdf)
        testing_t_errorf_v(t, "unexpected KDF ID: got %04x, want %04x",
                           (int)hpke_kdf_id(kdf), (int)v->kdf);
    const HpkeAEAD *aead = hpke_new_aead(v->aead, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    if (hpke_aead_id(aead) != v->aead)
        testing_t_errorf_v(t, "unexpected AEAD ID: got %04x, want %04x",
                           (int)hpke_aead_id(aead), (int)v->aead);
    const HpkeKEM *kem = hpke_new_kem(v->kem, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    if (hpke_kem_id(kem) != v->kem)
        testing_t_errorf_v(t, "unexpected KEM ID: got %04x, want %04x",
                           (int)hpke_kem_id(kem), (int)v->kem);

    Slice pub_key_bytes = unhex(a, v->pk_rm);
    HpkePublicKey *kem_sender = hpke_kem_new_public_key(kem, a, pub_key_bytes, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    if (hpke_public_key_kem(kem_sender) != kem)
        testing_t_errorf_v(t, "unexpected KEM from sender: got %04x, want %04x",
                           (int)hpke_kem_id(hpke_public_key_kem(kem_sender)),
                           (int)hpke_kem_id(kem));
    check_bytes(t, a, "KEM bytes", hpke_public_key_bytes(kem_sender, a), pub_key_bytes);

    HpkeEncapRandomness r;
    EcdhPrivateKey *eph;
    if (!setup_derandomized_encap(t, a, unhex(a, v->ikm_e), kem_sender, &r, &eph))
        testing_t_fatalf_v(t, "no randomness for KEM %04x", (int)v->kem);

    Slice info = unhex(a, v->info);
    HpkeSender *sender;
    Slice encap =
        burrow__hpke_new_sender_with(a, kem_sender, kdf, aead, info, &r, &sender, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    if (encap.len != burrow__hpke_kem_enc_size(kem))
        testing_t_errorf_v(t, "unexpected encapsulated key size: got %d, want %d",
                           encap.len, burrow__hpke_kem_enc_size(kem));
    check_bytes(t, a, "encapsulated key", encap, unhex(a, v->enc));

    Slice priv_key_bytes = unhex(a, v->sk_rm);
    HpkePrivateKey *kem_recipient =
        hpke_kem_new_private_key(kem, a, priv_key_bytes, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    if (hpke_private_key_kem(kem_recipient) != kem)
        testing_t_errorf_v(t, "unexpected KEM from recipient: got %04x, want %04x",
                           (int)hpke_kem_id(hpke_private_key_kem(kem_recipient)),
                           (int)hpke_kem_id(kem));
    Slice kem_recipient_bytes = hpke_private_key_bytes(kem_recipient, a, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    /* X25519 serialized keys must be clamped, so the bytes might not match. */
    bool x25519 = v->kem == hpke_kem_id(hpke_dhkem(ecdh_x25519()));
    if (!x25519)
        check_bytes(t, a, "KEM bytes", kem_recipient_bytes, priv_key_bytes);
    if (x25519) {
        HpkePrivateKey *kem2 =
            hpke_kem_new_private_key(kem, a, kem_recipient_bytes, &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "%s", error_text(err));
        Slice kem_recipient_bytes2 = hpke_private_key_bytes(kem2, a, &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "%s", error_text(err));
        check_bytes(t, a, "X25519 re-serialized key", kem_recipient_bytes2,
                    kem_recipient_bytes);
        check_bytes(t, a, "X25519 re-derived public key",
                    hpke_public_key_bytes(hpke_private_key_public_key(kem2), a),
                    pub_key_bytes);
        hpke_private_key_free(kem2);
    }
    check_bytes(t, a, "KEM sender bytes",
                hpke_public_key_bytes(hpke_private_key_public_key(kem_recipient), a),
                pub_key_bytes);

    HpkePrivateKey *deriv_recipient =
        hpke_kem_derive_key_pair(kem, a, unhex(a, v->ikm_r), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    Slice deriv_recipient_bytes = hpke_private_key_bytes(deriv_recipient, a, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    if (!x25519)
        check_bytes(t, a, "KEM bytes from seed", deriv_recipient_bytes, priv_key_bytes);
    check_bytes(t, a, "KEM sender bytes from seed",
                hpke_public_key_bytes(hpke_private_key_public_key(deriv_recipient), a),
                pub_key_bytes);

    HpkeRecipient *recipient =
        hpke_new_recipient(a, encap, kem_recipient, kdf, aead, info, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));

    if (aead != hpke_export_only() && v->acc_encryptions[0] != 0) {
        Sha3SHAKE *source = sha3_new_shake128(a), *sink = sha3_new_shake128(a);
        for (int i = 0; i < 1000; i++) {
            Slice aad = draw_random_input(a, source);
            Slice plaintext = draw_random_input(a, source);
            Slice ciphertext = hpke_sender_seal(sender, a, aad, plaintext, &err);
            if (BURROW_FAILED(err))
                testing_t_fatalf_v(t, "%s", error_text(err));
            sha3_shake_write(sink, ciphertext, NULL);
            Slice got = hpke_recipient_open(recipient, a, aad, ciphertext, &err);
            if (BURROW_FAILED(err))
                testing_t_fatalf_v(t, "%s", error_text(err));
            check_bytes(t, a, "plaintext", got, plaintext);
        }
        Byte encryptions[16];
        sha3_shake_read(sink, bs(encryptions, 16), NULL);
        check_bytes(t, a, "accumulated encryptions", bs(encryptions, 16),
                    unhex(a, v->acc_encryptions));
    } else if (aead != hpke_export_only()) {
        for (int i = 0; i < v->n_encryptions; i++) {
            const GenHpkeEncryption *enc = &v->encryptions[i];
            Slice aad = unhex(a, enc->aad);
            Slice plaintext = unhex(a, enc->pt);
            Slice ciphertext = hpke_sender_seal(sender, a, aad, plaintext, &err);
            if (BURROW_FAILED(err))
                testing_t_fatalf_v(t, "%s", error_text(err));
            check_bytes(t, a, "ciphertext", ciphertext, unhex(a, enc->ct));
            Slice got = hpke_recipient_open(recipient, a, aad, ciphertext, &err);
            if (BURROW_FAILED(err))
                testing_t_fatalf_v(t, "%s", error_text(err));
            check_bytes(t, a, "plaintext", got, plaintext);
        }
    } else {
        hpke_sender_seal(sender, a, slice_nil(TYPE_BYTE), slice_nil(TYPE_BYTE), &err);
        if (BURROW_OK(err))
            testing_t_errorf_v(t, "expected error from Seal with export-only AEAD");
        err = BURROW_NO_ERROR;
        hpke_recipient_open(recipient, a, slice_nil(TYPE_BYTE), slice_nil(TYPE_BYTE),
                            &err);
        if (BURROW_OK(err))
            testing_t_errorf_v(t, "expected error from Open with export-only AEAD");
        err = BURROW_NO_ERROR;
    }

    if (v->acc_exports[0] != 0) {
        Sha3SHAKE *source = sha3_new_shake128(a), *sink = sha3_new_shake128(a);
        for (Int l = 0; l < 1000; l++) {
            Slice c = draw_random_input(a, source);
            Str context = str_from_bytes(c.p, c.len);
            Slice value = hpke_sender_export(sender, a, context, l, &err);
            if (BURROW_FAILED(err))
                testing_t_fatalf_v(t, "%s", error_text(err));
            sha3_shake_write(sink, value, NULL);
            Slice got = hpke_recipient_export(recipient, a, context, l, &err);
            if (BURROW_FAILED(err))
                testing_t_fatalf_v(t, "%s", error_text(err));
            check_bytes(t, a, "recipient exported secret", got, value);
        }
        Byte exports[16];
        sha3_shake_read(sink, bs(exports, 16), NULL);
        check_bytes(t, a, "accumulated exports", bs(exports, 16),
                    unhex(a, v->acc_exports));
    } else {
        for (int i = 0; i < v->n_exports; i++) {
            const GenHpkeExport *exp = &v->exports[i];
            Slice c = unhex(a, exp->context);
            Str context = str_from_bytes(c.p, c.len);
            Slice value = hpke_sender_export(sender, a, context, exp->l, &err);
            if (BURROW_FAILED(err))
                testing_t_fatalf_v(t, "%s", error_text(err));
            check_bytes(t, a, "exported value", value, unhex(a, exp->value));
            Slice got = hpke_recipient_export(recipient, a, context, exp->l, &err);
            if (BURROW_FAILED(err))
                testing_t_fatalf_v(t, "%s", error_text(err));
            check_bytes(t, a, "recipient exported secret", got, value);
        }
    }

    hpke_private_key_free(deriv_recipient);
    hpke_private_key_free(kem_recipient);
    ecdh_private_key_free(eph);
    hpke_public_key_free(kem_sender);
    arena_free(&ar);
}

static void test_vectors(void *env, TestingT *t) {
    const char *file = env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    for (size_t i = 0; i < sizeof gen_hpke_vectors / sizeof gen_hpke_vectors[0]; i++) {
        const GenHpkeVector *v = &gen_hpke_vectors[i];
        if (strcmp(v->file, file) != 0)
            continue;
        testing_t_run(t,
                      fmt_sprintf_v(arena_allocator(&ar),
                                    "mode 0000 kem %04x kdf %04x aead %04x",
                                    (int)v->kem, (int)v->kdf, (int)v->aead),
                      BURROW_FN(TestingTFunc, test_vector, (void *)(uintptr_t)v));
    }
    arena_free(&ar);
}

static void TestVectors(TestingT *t) {
    testing_t_run(t, BURROW_S("rfc9180"),
                  BURROW_FN(TestingTFunc, test_vectors, (void *)(uintptr_t)"rfc9180"));
    testing_t_run(t, BURROW_S("hpke-pq"),
                  BURROW_FN(TestingTFunc, test_vectors, (void *)(uintptr_t)"hpke-pq"));
}

/* ------------------------------------------------------------- Singletons */

static void TestSingletons(TestingT *t) {
    if (hpke_hkdfsha256() != hpke_hkdfsha256())
        testing_t_errorf_v(t, "HKDFSHA256() != HKDFSHA256()");
    if (hpke_hkdfsha384() != hpke_hkdfsha384())
        testing_t_errorf_v(t, "HKDFSHA384() != HKDFSHA384()");
    if (hpke_hkdfsha512() != hpke_hkdfsha512())
        testing_t_errorf_v(t, "HKDFSHA512() != HKDFSHA512()");
    if (hpke_aes128_gcm() != hpke_aes128_gcm())
        testing_t_errorf_v(t, "AES128GCM() != AES128GCM()");
    if (hpke_aes256_gcm() != hpke_aes256_gcm())
        testing_t_errorf_v(t, "AES256GCM() != AES256GCM()");
    if (hpke_cha_cha20_poly1305() != hpke_cha_cha20_poly1305())
        testing_t_errorf_v(t, "ChaCha20Poly1305() != ChaCha20Poly1305()");
    if (hpke_export_only() != hpke_export_only())
        testing_t_errorf_v(t, "ExportOnly() != ExportOnly()");
    if (hpke_dhkem(ecdh_p256()) != hpke_dhkem(ecdh_p256()))
        testing_t_errorf_v(t, "DHKEM(P-256) != DHKEM(P-256)");
    if (hpke_dhkem(ecdh_p384()) != hpke_dhkem(ecdh_p384()))
        testing_t_errorf_v(t, "DHKEM(P-384) != DHKEM(P-384)");
    if (hpke_dhkem(ecdh_p521()) != hpke_dhkem(ecdh_p521()))
        testing_t_errorf_v(t, "DHKEM(P-521) != DHKEM(P-521)");
    if (hpke_dhkem(ecdh_x25519()) != hpke_dhkem(ecdh_x25519()))
        testing_t_errorf_v(t, "DHKEM(X25519) != DHKEM(X25519)");
    if (hpke_mlkem768() != hpke_mlkem768())
        testing_t_errorf_v(t, "MLKEM768() != MLKEM768()");
    if (hpke_mlkem1024() != hpke_mlkem1024())
        testing_t_errorf_v(t, "MLKEM1024() != MLKEM1024()");
    if (hpke_mlkem768_x25519() != hpke_mlkem768_x25519())
        testing_t_errorf_v(t, "MLKEM768X25519() != MLKEM768X25519()");
    if (hpke_mlkem768_p256() != hpke_mlkem768_p256())
        testing_t_errorf_v(t, "MLKEM768P256() != MLKEM768P256()");
    if (hpke_mlkem1024_p384() != hpke_mlkem1024_p384())
        testing_t_errorf_v(t, "MLKEM1024P384() != MLKEM1024P384()");
}

/* ----------------------------------------------------------------- burrow */

static void want_error(TestingT *t, const char *what, Error err, const char *want) {
    if (BURROW_OK(err)) {
        testing_t_errorf_v(t, "%s: no error, want %q", str_from_cstr(what),
                           str_from_cstr(want));
        return;
    }
    if (!str_eq(error_text(err), str_from_cstr(want)))
        testing_t_errorf_v(t, "%s: error %q, want %q", str_from_cstr(what),
                           error_text(err), str_from_cstr(want));
}

/* An EcdhPrivateKey behind a key exchanger that is not one, the way a key in
 * a hardware module would be. */
static const EcdhPublicKey *opaque_public_key(void *self) {
    return ecdh_private_key_public_key(self);
}

static const EcdhCurve *opaque_curve(void *self) {
    return ecdh_private_key_curve(self);
}

static Slice opaque_ecdh(void *self, Alloc *a, const EcdhPublicKey *remote,
                         Error *err) {
    return ecdh_private_key_ecdh(self, a, remote, err);
}

static const EcdhKeyExchangerVT opaque_vt = {TYPE_BYTE, opaque_public_key, opaque_curve,
                                             opaque_ecdh};

static void TestErrors(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;

    if (hpke_new_kem(0x0021, &err) != NULL)
        testing_t_errorf_v(t, "NewKEM(0x0021) is not NULL");
    want_error(t, "NewKEM(0x0021)", err, "unsupported KEM");
    err = BURROW_NO_ERROR;
    if (hpke_new_kdf(0x0012, &err) != NULL)
        testing_t_errorf_v(t, "NewKDF(0x0012) is not NULL");
    want_error(t, "NewKDF(0x0012)", err, "unsupported KDF 0012");
    err = BURROW_NO_ERROR;
    if (hpke_new_aead(0xabcd, &err) != NULL)
        testing_t_errorf_v(t, "NewAEAD(0xabcd) is not NULL");
    want_error(t, "NewAEAD(0xabcd)", err, "unsupported AEAD abcd");
    err = BURROW_NO_ERROR;

    const HpkeKDF *kdf = hpke_hkdfsha256();
    HpkePrivateKey *k = hpke_kem_generate_key(hpke_dhkem(ecdh_x25519()), a, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    const HpkePublicKey *pk = hpke_private_key_public_key(k);

    /* An export-only context seals and opens nothing, and exports up to
     * 0xffff bytes. */
    HpkeSender *s;
    Slice enc =
        hpke_new_sender(a, pk, kdf, hpke_export_only(), slice_nil(TYPE_BYTE), &s, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    HpkeRecipient *r = hpke_new_recipient(a, enc, k, kdf, hpke_export_only(),
                                          slice_nil(TYPE_BYTE), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    hpke_sender_seal(s, a, slice_nil(TYPE_BYTE), sb("x"), &err);
    want_error(t, "Sender.Seal", err, "export-only instantiation");
    err = BURROW_NO_ERROR;
    hpke_recipient_open(r, a, slice_nil(TYPE_BYTE), sb("x"), &err);
    want_error(t, "Recipient.Open", err, "export-only instantiation");
    err = BURROW_NO_ERROR;
    hpke_sender_export(s, a, BURROW_S(""), -1, &err);
    want_error(t, "Sender.Export(-1)", err, "invalid length");
    err = BURROW_NO_ERROR;
    hpke_recipient_export(r, a, BURROW_S(""), 0x10000, &err);
    want_error(t, "Recipient.Export(0x10000)", err, "invalid length");
    err = BURROW_NO_ERROR;
    Slice x = hpke_sender_export(s, a, BURROW_S(""), 0xffff, &err);
    Slice y = hpke_recipient_export(r, a, BURROW_S(""), 0xffff, &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Export(0xffff): %s", error_text(err));
    else if (x.len != 0xffff || !bytes_eq(x, y))
        testing_t_errorf_v(t, "Export(0xffff) gave %d bytes, or the two sides differ",
                           x.len);
    err = BURROW_NO_ERROR;
    /* SHAKE256 can export 0xffff bytes too. */
    HpkeSender *s2;
    Slice enc2 = hpke_new_sender(a, pk, hpke_shake256(), hpke_export_only(),
                                 slice_nil(TYPE_BYTE), &s2, &err);
    HpkeRecipient *r2 = hpke_new_recipient(
        a, enc2, k, hpke_shake256(), hpke_export_only(), slice_nil(TYPE_BYTE), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    x = hpke_sender_export(s2, a, BURROW_S("ctx"), 0xffff, &err);
    y = hpke_recipient_export(r2, a, BURROW_S("ctx"), 0xffff, &err);
    if (BURROW_FAILED(err) || x.len != 0xffff || !bytes_eq(x, y))
        testing_t_errorf_v(t, "SHAKE256 Export(0xffff) failed or the two sides differ");
    err = BURROW_NO_ERROR;

    Byte short_ct[31] = {0};
    hpke_open(a, k, kdf, hpke_aes128_gcm(), slice_nil(TYPE_BYTE), bs(short_ct, 31),
              &err);
    want_error(t, "Open(31 bytes)", err, "ciphertext too short");
    err = BURROW_NO_ERROR;

    /* The hybrids. */
    const HpkeKEM *xwing = hpke_mlkem768_x25519();
    Byte secret[32] = {1};
    hpke_kem_new_private_key(xwing, a, bs(secret, 31), &err);
    want_error(t, "X-Wing NewPrivateKey(31 bytes)", err,
               "hpke: invalid hybrid KEM secret length");
    err = BURROW_NO_ERROR;
    HpkePrivateKey *hk = hpke_kem_new_private_key(xwing, a, bs(secret, 32), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    Slice hpb = hpke_public_key_bytes(hpke_private_key_public_key(hk), a);
    hpke_kem_new_public_key(xwing, a, slice_sub(hpb, 0, hpb.len - 1), &err);
    want_error(t, "X-Wing NewPublicKey(short)", err, "invalid public key size");
    err = BURROW_NO_ERROR;
    hpke_new_recipient(a, slice_sub(hpb, 0, 100), hk, kdf, hpke_aes128_gcm(),
                       slice_nil(TYPE_BYTE), &err);
    want_error(t, "X-Wing NewRecipient(100 bytes)", err,
               "invalid encapsulated key size");
    err = BURROW_NO_ERROR;

    /* A hybrid of the wrong ML-KEM, or of a curve with none. */
    MlkemDecapsulationKey768 *dk768 = mlkem_generate_key768(a, &err);
    MlkemDecapsulationKey1024 *dk1024 = mlkem_generate_key1024(a, &err);
    EcdhPrivateKey *x25519 =
        ecdh_curve_generate_key(ecdh_x25519(), a, (IoReader){0}, &err);
    EcdhPrivateKey *p256 = ecdh_curve_generate_key(ecdh_p256(), a, (IoReader){0}, &err);
    EcdhPrivateKey *p384 = ecdh_curve_generate_key(ecdh_p384(), a, (IoReader){0}, &err);
    EcdhPrivateKey *p521 = ecdh_curve_generate_key(ecdh_p521(), a, (IoReader){0}, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    CryptoDecapsulator d768 = mlkem_decapsulation_key768_as_decapsulator(dk768);
    CryptoDecapsulator d1024 = mlkem_decapsulation_key1024_as_decapsulator(dk1024);
    CryptoEncapsulator e768 = crypto_decapsulator_encapsulator(d768);
    CryptoEncapsulator e1024 = crypto_decapsulator_encapsulator(d1024);
    struct {
        CryptoDecapsulator d;
        CryptoEncapsulator e;
        EcdhPrivateKey *t;
        const char *want;
    } hybrids[] = {
        {d1024, e1024, x25519, "invalid PQ KEM for X25519 hybrid"},
        {d1024, e1024, p256, "invalid PQ KEM for P-256 hybrid"},
        {d768, e768, p384, "invalid PQ KEM for P-384 hybrid"},
        {d768, e768, p521, "unsupported curve"},
    };
    for (size_t i = 0; i < sizeof hybrids / sizeof hybrids[0]; i++) {
        if (hpke_new_hybrid_public_key(a, hybrids[i].e,
                                       ecdh_private_key_public_key(hybrids[i].t),
                                       &err) != NULL)
            testing_t_errorf_v(t, "NewHybridPublicKey #%d is not NULL", (int)i);
        want_error(t, "NewHybridPublicKey", err, hybrids[i].want);
        err = BURROW_NO_ERROR;
        if (hpke_new_hybrid_private_key(a, hybrids[i].d,
                                        ecdh_private_key_key_exchanger(hybrids[i].t),
                                        &err) != NULL)
            testing_t_errorf_v(t, "NewHybridPrivateKey #%d is not NULL", (int)i);
        want_error(t, "NewHybridPrivateKey", err, hybrids[i].want);
        err = BURROW_NO_ERROR;
    }

    /* A hybrid made of its parts has no seed to give back, and works. */
    HpkePrivateKey *parts = hpke_new_hybrid_private_key(
        a, d768, ecdh_private_key_key_exchanger(x25519), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    if (hpke_private_key_kem(parts) != xwing)
        testing_t_errorf_v(t, "NewHybridPrivateKey(768, X25519) is not X-Wing");
    hpke_private_key_bytes(parts, a, &err);
    want_error(t, "hybrid of parts Bytes", err, "private key seed not available");
    err = BURROW_NO_ERROR;
    Slice ct = hpke_seal(a, hpke_private_key_public_key(parts), kdf, hpke_aes128_gcm(),
                         sb("info"), sb("hi"), &err);
    Slice pt = hpke_open(a, parts, kdf, hpke_aes128_gcm(), sb("info"), ct, &err);
    if (BURROW_FAILED(err) || !bytes_eq(pt, sb("hi")))
        testing_t_errorf_v(t, "hybrid of parts does not round trip");
    err = BURROW_NO_ERROR;

    /* An ML-KEM key made of a decapsulator gives its seed back. */
    HpkePrivateKey *mk = hpke_new_mlkem_private_key(a, d1024, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    if (hpke_private_key_kem(mk) != hpke_mlkem1024())
        testing_t_errorf_v(t, "NewMLKEMPrivateKey(1024) is not ML-KEM-1024");
    check_bytes(t, a, "ML-KEM seed", hpke_private_key_bytes(mk, a, &err),
                mlkem_decapsulation_key1024_bytes(dk1024, a));
    HpkePublicKey *mpk = hpke_new_mlkem_public_key(a, e1024, &err);
    if (BURROW_FAILED(err) || hpke_public_key_kem(mpk) != hpke_mlkem1024())
        testing_t_errorf_v(t, "NewMLKEMPublicKey(1024) failed");
    err = BURROW_NO_ERROR;

    /* A DHKEM key behind a key exchanger that is not an EcdhPrivateKey works
     * but has no bytes. */
    EcdhKeyExchanger opaque = {&opaque_vt, p256};
    HpkePrivateKey *ok = hpke_new_dhkem_private_key(a, opaque, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    if (hpke_private_key_kem(ok) != hpke_dhkem(ecdh_p256()))
        testing_t_errorf_v(t, "NewDHKEMPrivateKey(P-256) is not DHKEM(P-256)");
    hpke_private_key_bytes(ok, a, &err);
    want_error(t, "opaque key Bytes", err, "ecdh: private key does not support Bytes");
    err = BURROW_NO_ERROR;
    HpkePublicKey *opk =
        hpke_new_dhkem_public_key(a, ecdh_private_key_public_key(p256), &err);
    ct = hpke_seal(a, opk, kdf, hpke_cha_cha20_poly1305(), slice_nil(TYPE_BYTE),
                   sb("hi"), &err);
    pt = hpke_open(a, ok, kdf, hpke_cha_cha20_poly1305(), slice_nil(TYPE_BYTE), ct,
                   &err);
    if (BURROW_FAILED(err) || !bytes_eq(pt, sb("hi")))
        testing_t_errorf_v(t, "opaque DHKEM key does not round trip");

    hpke_public_key_free(opk);
    hpke_private_key_free(ok);
    hpke_public_key_free(mpk);
    hpke_private_key_free(mk);
    hpke_private_key_free(parts);
    ecdh_private_key_free(p521);
    ecdh_private_key_free(p384);
    ecdh_private_key_free(p256);
    ecdh_private_key_free(x25519);
    mlkem_decapsulation_key1024_free(dk1024);
    mlkem_decapsulation_key768_free(dk768);
    hpke_private_key_free(hk);
    hpke_private_key_free(k);
    arena_free(&ar);
}

/* Keys on the heap: everything they own goes back with them, which ASan and
 * the leak checker see. */
static void TestHeapKeys(TestingT *t) {
    Alloc *h = heap_allocator();
    const HpkeKEM *kems[9];
    size_t n = all_kems(kems);
    for (size_t i = 0; i < n; i++) {
        Error err = BURROW_NO_ERROR;
        HpkePrivateKey *k = hpke_kem_generate_key(kems[i], h, &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "%s", error_text(err));
        Slice kb = hpke_private_key_bytes(k, h, &err);
        Slice pkb = hpke_public_key_bytes(hpke_private_key_public_key(k), h);
        HpkePrivateKey *kk = hpke_kem_new_private_key(kems[i], h, kb, &err);
        HpkePublicKey *pk = hpke_kem_new_public_key(kems[i], h, pkb, &err);
        HpkePrivateKey *dk = hpke_kem_derive_key_pair(kems[i], h, pkb, &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "%s", error_text(err));
        Slice ct = hpke_seal(h, pk, hpke_hkdfsha256(), hpke_aes128_gcm(), sb("info"),
                             sb("plaintext"), &err);
        Slice pt = hpke_open(h, kk, hpke_hkdfsha256(), hpke_aes128_gcm(), sb("info"),
                             ct, &err);
        if (BURROW_FAILED(err) || !bytes_eq(pt, sb("plaintext")))
            testing_t_errorf_v(t, "KEM %04x: heap keys do not round trip",
                               (int)hpke_kem_id(kems[i]));
        mem_free(h, pt.p, (size_t)pt.cap, 1);
        mem_free(h, ct.p, (size_t)ct.cap, 1);
        mem_free(h, pkb.p, (size_t)pkb.cap, 1);
        mem_free(h, kb.p, (size_t)kb.cap, 1);
        hpke_private_key_free(dk);
        hpke_public_key_free(pk);
        hpke_private_key_free(kk);
        hpke_private_key_free(k);
    }
}

/* ------------------------------------------------------------- benchmarks */

typedef struct SuiteBench {
    const HpkeKEM *kem;
    HpkePrivateKey *k;
} SuiteBench;

static void bench_seal_open(void *env, TestingB *b) {
    const SuiteBench *s = env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    const HpkePublicKey *pk = hpke_private_key_public_key(s->k);
    for (Int i = 0; i < testing_b_n(b); i++) {
        Error err = BURROW_NO_ERROR;
        Slice ct = hpke_seal(a, pk, hpke_hkdfsha256(), hpke_aes128_gcm(), sb("info"),
                             sb("plaintext"), &err);
        hpke_open(a, s->k, hpke_hkdfsha256(), hpke_aes128_gcm(), sb("info"), ct, &err);
        if (BURROW_FAILED(err))
            testing_b_fatalf_v(b, "%s", error_text(err));
        arena_reset(&ar);
    }
    arena_free(&ar);
}

/* burrow's: one Seal and one Open for each KEM, with HKDF-SHA256 and
 * AES-128-GCM. */
static void BenchmarkSealOpen(TestingB *b) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    const HpkeKEM *kems[9];
    size_t n = all_kems(kems);
    for (size_t i = 0; i < n; i++) {
        Error err = BURROW_NO_ERROR;
        SuiteBench s = {kems[i], hpke_kem_generate_key(kems[i], a, &err)};
        if (BURROW_FAILED(err))
            testing_b_fatalf_v(b, "%s", error_text(err));
        testing_b_run(b, fmt_sprintf_v(a, "KEM_%04x", (int)hpke_kem_id(kems[i])),
                      BURROW_FN(TestingBFunc, bench_seal_open, &s));
        hpke_private_key_free(s.k);
    }
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestExample)                                                                     \
    X(TestRoundTrip)                                                                   \
    X(TestVectors)                                                                     \
    X(TestSingletons)                                                                  \
    X(TestErrors)                                                                      \
    X(TestHeapKeys)                                                                    \
    X(BenchmarkSealOpen)

TESTING_MAIN(TESTS)
