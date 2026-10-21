/* ctgrind: the crypto code that Go says runs in constant time, run under
 * valgrind's memcheck with its secrets marked undefined.
 *
 * Memcheck reports a branch or a memory address that depends on undefined
 * bytes. Marking a key undefined before handing it over turns that into a
 * report of every place the code branches or indexes on the key, which is
 * the technique from https://www.imperialviolet.org/2010/04/01/ctgrind.html.
 * Outside valgrind the marks do nothing and the program just runs.
 *
 * Each check marks its secrets with secret() and marks what is meant to be
 * public again with public_(), and the program fails only through valgrind's
 * --error-exitcode. The places where Go branches on a secret on purpose, to
 * return an error that only says the secret was invalid, are listed with the
 * reason in tools/ctgrind.supp.
 *
 * Not in Go. */

#include "burrow/burrow.h"
#include "burrow/crypto/aes.h"
#include "burrow/crypto/cipher.h"
#include "burrow/crypto/ecdh.h"
#include "burrow/crypto/ecdsa.h"
#include "burrow/crypto/ed25519.h"
#include "burrow/crypto/elliptic.h"
#include "burrow/crypto/hmac.h"
#include "burrow/crypto/mlkem.h"
#include "burrow/crypto/rand.h"
#include "burrow/crypto/sha256.h"
#include "burrow/crypto/sha512.h"
#include "burrow/crypto/subtle.h"
#include "burrow/mem/arena.h"

#include "../src/crypto/aes_internal.h"
#include "../src/crypto/bigmod.h"
#include "../src/crypto/chacha20poly1305.h"
#include "../src/crypto/ecdsa_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <valgrind/memcheck.h>

static Arena arena;

static Slice bytes(Byte *p, Int n) {
    return slice_from(p, n, n, TYPE_BYTE);
}

/* Fills b from crypto/rand, which valgrind sees as defined, and then marks it
 * as the secret it is. */
static Slice secret_bytes(Byte *p, Int n) {
    Slice b = bytes(p, n);
    crypto_rand_read(b, NULL);
    VALGRIND_MAKE_MEM_UNDEFINED(p, (size_t)n);
    return b;
}

static void secret(void *p, size_t n) {
    VALGRIND_MAKE_MEM_UNDEFINED(p, n);
}

static void public_(void *p, size_t n) {
    VALGRIND_MAKE_MEM_DEFINED(p, n);
}

static void check_failed(const char *name, Error err) {
    if (!BURROW_FAILED(err))
        return;
    Str s = error_text(err);
    fprintf(stderr, "ctgrind: %s: %.*s\n", name, (int)s.len, (const char *)s.p);
    exit(2);
}

static void begin(void) {
    arena_init(&arena, NULL, 0);
}

static void end(const char *name) {
    arena_free(&arena);
    printf("ok\t%s\n", name);
}

static void check_subtle(void) {
    Byte x[64], y[64], d[64];
    begin();
    Slice xs = secret_bytes(x, 64);
    Slice ys = secret_bytes(y, 64);
    Int r = subtle_constant_time_compare(xs, ys);
    r += subtle_constant_time_byte_eq(x[0], y[0]);
    r += subtle_constant_time_eq((int32_t)x[1], (int32_t)y[1]);
    r += subtle_constant_time_less_or_eq((Int)x[2], (Int)y[2]);
    r += subtle_constant_time_select((Int)(x[3] & 1), (Int)x[4], (Int)y[4]);
    subtle_constant_time_copy((Int)(x[5] & 1), xs, ys);
    subtle_xor_bytes(bytes(d, 64), xs, ys);
    r += hmac_equal(xs, ys);
    public_(&r, sizeof r);
    end("crypto/subtle");
}

static void check_hashes(void) {
    Byte key[32], msg[200];
    begin();
    Alloc *a = arena_allocator(&arena);
    Slice k = secret_bytes(key, 32);
    Slice m = secret_bytes(msg, 200);
    Sha256Sum256Ret s256 = sha256_sum256(m);
    Sha512Sum512Ret s512 = sha512_sum512(m);
    Hash h = hmac_new(a, sha256_new, k);
    hash_write(h, m, NULL);
    Slice mac = hash_sum(a, h, (Slice){0});
    public_(&s256, sizeof s256);
    public_(&s512, sizeof s512);
    public_(mac.p, (size_t)mac.len);
    end("crypto/sha256, crypto/sha512 and crypto/hmac");
}

/* AES with each key size, on a secret block, the portable code or the AES
 * instructions as hardware says, and GCM over it. The ciphertext is public, and
 * so is whether Open accepts it. */
static void check_aes(const char *name, bool portable) {
    Byte key[32], pt[100], block[16];
    Byte nonce[12] = {0};
    Byte ad[] = "additional data that is not secret";
    Error err = {0};
    begin();
    Alloc *a = arena_allocator(&arena);
    burrow__aes_set_portable(portable);
    for (Int n = 16; n <= 32; n += 8) {
        CipherBlock b = aes_new_cipher(a, secret_bytes(key, n), &err);
        check_failed(name, err);
        Slice bs = secret_bytes(block, 16);
        cipher_block_encrypt(b, bs, bs);
        cipher_block_decrypt(b, bs, bs);
        public_(block, sizeof block);
        CipherAEAD g = cipher_new_gcm(a, b, &err);
        check_failed(name, err);
        Slice ads = bytes(ad, (Int)sizeof ad - 1);
        Slice ct = cipher_aead_seal(g, a, (Slice){0}, bytes(nonce, 12),
                                    secret_bytes(pt, (Int)sizeof pt), ads);
        public_(ct.p, (size_t)ct.len);
        Slice out = cipher_aead_open(g, a, (Slice){0}, bytes(nonce, 12), ct, ads, &err);
        check_failed(name, err);
        public_(out.p, (size_t)out.len);
    }
    burrow__aes_set_portable(false);
    end(name);
}

/* ChaCha20-Poly1305 and XChaCha20-Poly1305, which have only the portable code. */
static void check_chacha20poly1305(void) {
    Byte key[CHACHA20_KEY_SIZE], pt[100];
    Byte nonce[24] = {0};
    Byte ad[] = "additional data that is not secret";
    Error err = {0};
    begin();
    Alloc *a = arena_allocator(&arena);
    for (int x = 0; x < 2; x++) {
        Slice k = secret_bytes(key, (Int)sizeof key);
        CipherAEAD c = x == 0 ? chacha20poly1305_new(a, k, &err)
                              : chacha20poly1305_new_x(a, k, &err);
        check_failed("crypto/internal/chacha20poly1305", err);
        Slice ns = bytes(nonce, cipher_aead_nonce_size(c));
        Slice ads = bytes(ad, (Int)sizeof ad - 1);
        Slice ct = cipher_aead_seal(c, a, (Slice){0}, ns,
                                    secret_bytes(pt, (Int)sizeof pt), ads);
        public_(ct.p, (size_t)ct.len);
        Slice out = cipher_aead_open(c, a, (Slice){0}, ns, ct, ads, &err);
        check_failed("crypto/internal/chacha20poly1305", err);
        public_(out.p, (size_t)out.len);
    }
    end("crypto/internal/chacha20poly1305");
}

static void check_ed25519(void) {
    Byte seed[ED25519_SEED_SIZE];
    Byte msg[] = "a message that is not secret";
    begin();
    Alloc *a = arena_allocator(&arena);
    Ed25519PrivateKey priv =
        ed25519_new_key_from_seed(a, secret_bytes(seed, sizeof seed));
    Slice sig = ed25519_sign(a, priv, bytes(msg, (Int)sizeof msg - 1));
    public_(sig.p, (size_t)sig.len);
    end("crypto/ed25519");
}

static void check_ecdh(const char *name, const EcdhCurve *c) {
    Byte key[66];
    Error err = {0};
    begin();
    Alloc *a = arena_allocator(&arena);
    EcdhPrivateKey *peer = ecdh_curve_generate_key(c, a, crypto_rand_reader, &err);
    check_failed(name, err);
    /* A key from generate_key, because random bytes are not always below the
     * order, and for P-521 they hardly ever are. */
    EcdhPrivateKey *g = ecdh_curve_generate_key(c, a, crypto_rand_reader, &err);
    check_failed(name, err);
    Slice gb = ecdh_private_key_bytes(g, a);
    memcpy(key, gb.p, (size_t)gb.len);
    secret(key, (size_t)gb.len);
    EcdhPrivateKey *k = ecdh_curve_new_private_key(c, a, bytes(key, gb.len), &err);
    public_(&err, sizeof err);
    public_(&k, sizeof k);
    check_failed(name, err);
    Slice shared = ecdh_private_key_ecdh(k, a, ecdh_private_key_public_key(peer), &err);
    public_(&err, sizeof err);
    public_(&shared, sizeof shared);
    check_failed(name, err);
    public_(shared.p, (size_t)shared.len);
    ecdh_private_key_free(k);
    ecdh_private_key_free(g);
    ecdh_private_key_free(peer);
    end(name);
}

/* ECDSA's sign, the one under the FIPS module, with a secret scalar and a
 * DRBG seeded with secret entropy for the nonce. The conversion from the
 * big.Int in a PrivateKey is left out, because math/big is not constant time,
 * in Go either. */
static void check_ecdsa(const char *name, EllipticCurve curve,
                        const EcdsaFipsCurve *c) {
    Byte key[66], entropy[48], hash[32] = {1, 2, 3};
    uint8_t r[66], s[66];
    Error err = {0};
    begin();
    Alloc *a = arena_allocator(&arena);
    EcdsaPrivateKey *k = ecdsa_generate_key(a, curve, crypto_rand_reader, &err);
    check_failed(name, err);
    Slice kb = ecdsa_private_key_bytes(k, a, &err);
    check_failed(name, err);
    memcpy(key, kb.p, (size_t)kb.len);
    secret(key, (size_t)kb.len);
    EcdsaDrbg *drbg = burrow__ecdsa_new_drbg(
        a, sha512_new, secret_bytes(entropy, (Int)sizeof entropy), slice_nil(TYPE_BYTE),
        slice_nil(TYPE_BYTE));
    bool ok = burrow__ecdsa_sign_with_drbg(c, bytes(key, kb.len), drbg,
                                           bytes(hash, (Int)sizeof hash), r, s, &err);
    public_(&ok, sizeof ok);
    public_(&err, sizeof err);
    check_failed(name, err);
    public_(r, sizeof r);
    public_(s, sizeof s);
    ecdsa_private_key_free(k);
    end(name);
}

static void check_mlkem(void) {
    Byte seed[MLKEM_SEED_SIZE];
    Error err = {0};
    begin();
    Alloc *a = arena_allocator(&arena);
    MlkemDecapsulationKey768 *dk =
        mlkem_new_decapsulation_key768(a, secret_bytes(seed, sizeof seed), &err);
    public_(&err, sizeof err);
    public_(&dk, sizeof dk);
    check_failed("crypto/mlkem", err);
    /* The encapsulation key is public, though it comes from the seed. */
    Slice ekb = mlkem_encapsulation_key768_bytes(
        mlkem_decapsulation_key768_encapsulation_key(dk), a);
    public_(ekb.p, (size_t)ekb.len);
    MlkemEncapsulationKey768 *ek = mlkem_new_encapsulation_key768(a, ekb, &err);
    check_failed("crypto/mlkem", err);
    Slice ct = {0};
    Slice k1 = mlkem_encapsulation_key768_encapsulate(ek, a, &ct);
    public_(ct.p, (size_t)ct.len);
    Slice k2 = mlkem_decapsulation_key768_decapsulate(dk, a, ct, &err);
    public_(&err, sizeof err);
    check_failed("crypto/mlkem", err);
    public_(k1.p, (size_t)k1.len);
    public_(k2.p, (size_t)k2.len);
    mlkem_encapsulation_key768_free(ek);
    mlkem_decapsulation_key768_free(dk);
    end("crypto/mlkem");
}

/* The exponentiation that RSA signing and decryption are made of, with a
 * secret base and exponent and a public 2048 bit modulus. */
static void check_bigmod(void) {
    Byte mb[256], xb[256], eb[256];
    Error err = {0};
    begin();
    Alloc *a = arena_allocator(&arena);
    crypto_rand_read(bytes(mb, 256), NULL);
    mb[0] |= 0x80;
    mb[255] |= 1;
    BigmodModulus *m = bigmod_new_modulus(a, bytes(mb, 256), &err);
    check_failed("crypto/internal/fips140/bigmod", err);
    crypto_rand_read(bytes(xb, 256), NULL);
    xb[0] &= 0x7f;
    BigmodNat x, out;
    bigmod_nat_init(&x, a);
    bigmod_nat_init(&out, a);
    bigmod_nat_set_bytes(&x, bytes(xb, 256), m, &err);
    check_failed("crypto/internal/fips140/bigmod", err);
    secret(x.limbs, (size_t)x.len * sizeof(Uint));
    bigmod_nat_exp(&out, &x, secret_bytes(eb, 256), m);
    Slice r = bigmod_nat_bytes(&out, a, m);
    public_(r.p, (size_t)r.len);
    end("crypto/internal/fips140/bigmod");
}

int main(void) {
    check_subtle();
    check_hashes();
    check_aes("crypto/aes and GCM, portable", true);
    bool hw = false;
    if (burrow__aes_hardware(&hw) != NULL && hw)
        check_aes("crypto/aes and GCM, AES instructions", false);
    check_chacha20poly1305();
    check_ed25519();
    check_ecdh("crypto/ecdh X25519", ecdh_x25519());
    check_ecdh("crypto/ecdh P-256", ecdh_p256());
    check_ecdh("crypto/ecdh P-384", ecdh_p384());
    check_ecdh("crypto/ecdh P-521", ecdh_p521());
    check_ecdsa("crypto/ecdsa P-224", elliptic_p224(), burrow__ecdsa_fips_p224());
    check_ecdsa("crypto/ecdsa P-256", elliptic_p256(), burrow__ecdsa_fips_p256());
    check_ecdsa("crypto/ecdsa P-384", elliptic_p384(), burrow__ecdsa_fips_p384());
    check_ecdsa("crypto/ecdsa P-521", elliptic_p521(), burrow__ecdsa_fips_p521());
    check_mlkem();
    check_bigmod();
    return 0;
}
