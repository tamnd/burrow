/* dudect: the crypto code that Go says runs in constant time, timed, with a
 * statistical test on whether the time depends on the secret.
 *
 * This is the method of "Dude, is my code constant time?" by Reparaz, Balasch
 * and Verbauwhede (https://eprint.iacr.org/2016/1123). Each check runs one
 * operation many times, with inputs from two classes picked at random for
 * each run: class 0 is one fixed input and class 1 is a fresh random one. It
 * reads the cycle counter around every call, and then runs Welch's t-test on
 * the two classes' times, once on all of them and once with the slowest runs
 * cut off at each of a series of percentiles, as the paper does. A |t| above
 * 10 fails the check, which is the paper's line for code that is clearly not
 * constant time. Below that, the test found nothing, which is not a proof:
 * more runs can find a smaller leak.
 *
 * The first check is a comparison that returns at the first byte that
 * differs. It has to fail. If it doesn't, the timer or the machine is too
 * noisy for the other checks to mean anything, and the program says so and
 * fails.
 *
 * The counts can be scaled with the first argument, so "dudect 10" runs ten
 * times as many. Where ctgrind finds a branch or an index on a secret on any
 * machine, this finds whatever this machine's timing shows, including what a
 * compiler or a CPU adds, but only for leaks large enough to see in that many
 * runs.
 *
 * Not in Go. */

#include "burrow/burrow.h"
#include "burrow/crypto/aes.h"
#include "burrow/crypto/cipher.h"
#include "burrow/crypto/ecdh.h"
#include "burrow/crypto/ed25519.h"
#include "burrow/crypto/mlkem.h"
#include "burrow/crypto/rand.h"
#include "burrow/crypto/subtle.h"
#include "burrow/math.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"

#include "../src/crypto/aes_internal.h"
#include "../src/crypto/bigmod.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* How far |t| can go before a check fails. */
#define T_FAIL 10.0

/* The number of percentiles the times are cut at. */
#define CROPS 100

/* How many inputs are made at a time, before they are timed. */
#define BATCH 1000

static Arena arena;

/* Where the result of each call goes, so that it can't be optimised away. */
static volatile uint64_t sink;

static uint64_t ticks(void) {
#if (defined(__GNUC__) || defined(__clang__)) &&                                       \
    (defined(__x86_64__) || defined(__i386__))
    return __builtin_ia32_rdtsc();
#elif (defined(__GNUC__) || defined(__clang__)) && defined(__aarch64__)
    uint64_t v;
    __asm__ volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(v) : : "memory");
    return v;
#else
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (uint64_t)ts.tv_sec * 1000000000U + (uint64_t)ts.tv_nsec;
#endif
}

static Slice bytes(Byte *p, Int n) {
    return slice_from(p, n, n, TYPE_BYTE);
}

static void random_bytes(Byte *p, size_t n) {
    crypto_rand_read(bytes(p, (Int)n), NULL);
}

static void must(const char *name, Error err) {
    if (!BURROW_FAILED(err))
        return;
    Str s = error_text(err);
    fprintf(stderr, "dudect: %s: %.*s\n", name, (int)s.len, (const char *)s.p);
    exit(2);
}

/* One check: the input for class 0 is fixed, set by setup, and run does the
 * operation on one input. */
typedef struct Check {
    const char *name;
    Int runs;
    size_t size;
    void (*setup)(Byte *fixed);
    void (*run)(Byte *in);
} Check;

/* Welch's t-test, kept with Welford's running mean and variance. */
typedef struct Welch {
    double n[2], mean[2], m2[2];
} Welch;

static void welch_add(Welch *w, int c, double x) {
    w->n[c]++;
    double d = x - w->mean[c];
    w->mean[c] += d / w->n[c];
    w->m2[c] += d * (x - w->mean[c]);
}

static double welch_t(const Welch *w) {
    if (w->n[0] < 2 || w->n[1] < 2)
        return 0;
    double v0 = w->m2[0] / (w->n[0] - 1), v1 = w->m2[1] / (w->n[1] - 1);
    double den = math_sqrt(v0 / w->n[0] + v1 / w->n[1]);
    return den == 0 ? 0 : (w->mean[0] - w->mean[1]) / den;
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

/* The largest |t| over all the times and each cut of them. */
static double max_t(const uint64_t *t, const Byte *cls, Int n) {
    uint64_t *sorted = malloc((size_t)n * sizeof *sorted);
    if (sorted == NULL) {
        fprintf(stderr, "dudect: out of memory\n");
        exit(2);
    }
    memcpy(sorted, t, (size_t)n * sizeof *sorted);
    qsort(sorted, (size_t)n, sizeof *sorted, cmp_u64);
    double best = 0;
    for (int k = -1; k < CROPS; k++) {
        /* The cuts from the paper's code: 1 - 0.5^(10(k+1)/CROPS). */
        uint64_t limit = UINT64_MAX;
        if (k >= 0) {
            double p = 1 - math_exp2(-10.0 * (k + 1) / CROPS);
            limit = sorted[(Int)(p * (double)n)];
        }
        Welch w;
        memset(&w, 0, sizeof w);
        for (Int i = 0; i < n; i++)
            if (t[i] < limit)
                welch_add(&w, cls[i], (double)t[i]);
        double v = welch_t(&w);
        if (v < 0)
            v = -v;
        if (v > best)
            best = v;
    }
    free(sorted);
    return best;
}

/* Times c, and gives the largest |t|. */
static double measure(const Check *c, Int scale) {
    Int n = c->runs * scale;
    Byte *fixed = malloc(c->size);
    Byte *in = malloc((size_t)BATCH * c->size);
    Byte *cls = malloc((size_t)n);
    uint64_t *t = malloc((size_t)n * sizeof *t);
    if (fixed == NULL || in == NULL || cls == NULL || t == NULL) {
        fprintf(stderr, "dudect: out of memory\n");
        exit(2);
    }
    arena_init(&arena, NULL, 0);
    c->setup(fixed);
    random_bytes(cls, (size_t)n);
    /* A first batch that isn't counted, to warm the caches up. */
    for (Int i = 0; i < BATCH; i++) {
        c->run(fixed);
        arena_reset(&arena);
    }
    for (Int done = 0; done < n; done += BATCH) {
        Int b = n - done < BATCH ? n - done : BATCH;
        for (Int i = 0; i < b; i++) {
            cls[done + i] &= 1;
            Byte *p = in + (size_t)i * c->size;
            if (cls[done + i] == 0)
                memcpy(p, fixed, c->size);
            else
                random_bytes(p, c->size);
        }
        for (Int i = 0; i < b; i++) {
            uint64_t t0 = ticks();
            c->run(in + (size_t)i * c->size);
            uint64_t t1 = ticks();
            t[done + i] = t1 - t0;
            arena_reset(&arena);
        }
    }
    double v = max_t(t, cls, n);
    arena_free(&arena);
    free(t);
    free(cls);
    free(in);
    free(fixed);
    return v;
}

/* ------------------------------------------------------------------ checks */

static Byte secret32[32];

static void setup_compare(Byte *fixed) {
    random_bytes(secret32, sizeof secret32);
    memcpy(fixed, secret32, sizeof secret32);
}

/* A comparison that returns at the first difference, which is what the
 * checks are meant to catch. */
static void run_leaky_compare(Byte *in) {
    int eq = 1;
    for (size_t i = 0; i < sizeof secret32; i++) {
        if (in[i] != secret32[i]) {
            eq = 0;
            break;
        }
    }
    sink = (uint64_t)eq;
}

static void run_compare(Byte *in) {
    sink = (uint64_t)subtle_constant_time_compare(bytes(in, 32), bytes(secret32, 32));
}

static void setup_fixed(Byte *fixed, size_t n) {
    random_bytes(fixed, n);
}

static void setup_32(Byte *fixed) {
    setup_fixed(fixed, 32);
}

static void setup_16(Byte *fixed) {
    setup_fixed(fixed, 16);
}

/* The portable AES code, encrypting a fixed or a random block under a key
 * that stays the same. */
static CipherBlock aes_block;

static void setup_aes(Byte *fixed) {
    Byte key[16];
    Error err = {0};
    random_bytes(key, sizeof key);
    burrow__aes_set_portable(true);
    aes_block = aes_new_cipher(heap_allocator(), bytes(key, 16), &err);
    must("crypto/aes", err);
    setup_16(fixed);
}

static void run_aes(Byte *in) {
    Byte out[16];
    cipher_block_encrypt(aes_block, bytes(out, 16), bytes(in, 16));
    sink = out[0];
}

/* AES-GCM on the portable code, from the key up: the key schedule, the GHASH
 * key and sealing 64 bytes. */
static void run_gcm(Byte *in) {
    Byte nonce[12] = {0}, pt[64] = {0};
    Error err = {0};
    Alloc *a = arena_allocator(&arena);
    CipherBlock b = aes_new_cipher(a, bytes(in, 16), &err);
    CipherAEAD g = cipher_new_gcm(a, b, &err);
    Slice ct =
        cipher_aead_seal(g, a, (Slice){0}, bytes(nonce, 12), bytes(pt, 64), (Slice){0});
    sink = ((const Byte *)ct.p)[0];
}

static const EcdhPublicKey *x25519_peer, *p256_peer;

static void setup_x25519(Byte *fixed) {
    Error err = {0};
    EcdhPrivateKey *k = ecdh_curve_generate_key(ecdh_x25519(), heap_allocator(),
                                                crypto_rand_reader, &err);
    must("crypto/ecdh X25519", err);
    x25519_peer = ecdh_private_key_public_key(k);
    setup_32(fixed);
}

static void run_ecdh(const EcdhCurve *c, const EcdhPublicKey *peer, Byte *in) {
    Error err = {0};
    Alloc *a = arena_allocator(&arena);
    EcdhPrivateKey *k = ecdh_curve_new_private_key(c, a, bytes(in, 32), &err);
    Slice s = ecdh_private_key_ecdh(k, a, peer, &err);
    sink = (uint64_t)s.len;
}

static void run_x25519(Byte *in) {
    run_ecdh(ecdh_x25519(), x25519_peer, in);
}

static void setup_p256(Byte *fixed) {
    Error err = {0};
    EcdhPrivateKey *k = ecdh_curve_generate_key(ecdh_p256(), heap_allocator(),
                                                crypto_rand_reader, &err);
    must("crypto/ecdh P-256", err);
    p256_peer = ecdh_private_key_public_key(k);
    setup_32(fixed);
}

static void run_p256(Byte *in) {
    run_ecdh(ecdh_p256(), p256_peer, in);
}

static void run_ed25519(Byte *in) {
    Byte msg[] = "a message that is not secret";
    Alloc *a = arena_allocator(&arena);
    Ed25519PrivateKey priv = ed25519_new_key_from_seed(a, bytes(in, ED25519_SEED_SIZE));
    Slice sig = ed25519_sign(a, priv, bytes(msg, (Int)sizeof msg - 1));
    sink = ((const Byte *)sig.p)[0];
}

/* ML-KEM-768 decapsulation under one key, of a valid ciphertext for class 0
 * and of random bytes for class 1, which the implicit rejection turns into a
 * random key. */
static MlkemDecapsulationKey768 *mlkem_dk;

static void setup_mlkem(Byte *fixed) {
    Byte seed[MLKEM_SEED_SIZE];
    Error err = {0};
    random_bytes(seed, sizeof seed);
    mlkem_dk = mlkem_new_decapsulation_key768(heap_allocator(),
                                              bytes(seed, sizeof seed), &err);
    must("crypto/mlkem", err);
    const MlkemEncapsulationKey768 *ek =
        mlkem_decapsulation_key768_encapsulation_key(mlkem_dk);
    Slice ct = {0};
    (void)mlkem_encapsulation_key768_encapsulate(ek, arena_allocator(&arena), &ct);
    memcpy(fixed, ct.p, MLKEM_CIPHERTEXT_SIZE768);
}

static void run_mlkem(Byte *in) {
    Error err = {0};
    Slice k = mlkem_decapsulation_key768_decapsulate(
        mlkem_dk, arena_allocator(&arena), bytes(in, MLKEM_CIPHERTEXT_SIZE768), &err);
    sink = ((const Byte *)k.p)[0];
}

/* bigmod's Exp with a 2048 bit modulus and base that stay the same, and a
 * fixed or a random 2048 bit exponent. */
static BigmodModulus *exp_m;
static BigmodNat exp_x;

static void setup_exp(Byte *fixed) {
    Byte mb[256], xb[256];
    Error err = {0};
    random_bytes(mb, sizeof mb);
    mb[0] |= 0x80;
    mb[255] |= 1;
    exp_m = bigmod_new_modulus(heap_allocator(), bytes(mb, 256), &err);
    must("crypto/internal/fips140/bigmod", err);
    random_bytes(xb, sizeof xb);
    xb[0] &= 0x7f;
    bigmod_nat_init(&exp_x, heap_allocator());
    bigmod_nat_set_bytes(&exp_x, bytes(xb, 256), exp_m, &err);
    must("crypto/internal/fips140/bigmod", err);
    setup_fixed(fixed, 256);
}

static void run_exp(Byte *in) {
    BigmodNat out;
    bigmod_nat_init(&out, arena_allocator(&arena));
    bigmod_nat_exp(&out, &exp_x, bytes(in, 256), exp_m);
    sink = out.limbs[0];
}

static const Check checks[] = {
    {"crypto/subtle ConstantTimeCompare", 1000000, 32, setup_compare, run_compare},
    {"crypto/aes, portable", 1000000, 16, setup_aes, run_aes},
    {"crypto/aes GCM, portable", 200000, 16, setup_16, run_gcm},
    {"crypto/ecdh X25519", 20000, 32, setup_x25519, run_x25519},
    {"crypto/ecdh P-256", 20000, 32, setup_p256, run_p256},
    {"crypto/ed25519", 20000, ED25519_SEED_SIZE, setup_32, run_ed25519},
    {"crypto/mlkem decapsulation", 50000, MLKEM_CIPHERTEXT_SIZE768, setup_mlkem,
     run_mlkem},
    {"crypto/internal/fips140/bigmod Exp", 10000, 256, setup_exp, run_exp},
};

int main(int argc, char **argv) {
    Int scale = 1;
    if (argc > 1)
        scale = (Int)atoi(argv[1]);
    if (scale < 1) {
        fprintf(stderr, "usage: dudect [scale]\n");
        return 2;
    }

    /* The self check: a leak this plain has to show. */
    const Check leaky = {"a comparison that returns early", 1000000, 32, setup_compare,
                         run_leaky_compare};
    double t = measure(&leaky, scale);
    if (t <= T_FAIL) {
        printf("FAIL\t%s\t|t| %.1f\n", leaky.name, t);
        fprintf(stderr, "dudect: the timer can't see a comparison that returns early, "
                        "so the checks can't be trusted on this machine\n");
        return 1;
    }
    printf("ok\t%s, found as it should be\t|t| %.1f\n", leaky.name, t);

    int failed = 0;
    for (size_t i = 0; i < sizeof checks / sizeof checks[0]; i++) {
        t = measure(&checks[i], scale);
        bool ok = t <= T_FAIL;
        printf("%s\t%s\t|t| %.1f\n", ok ? "ok" : "FAIL", checks[i].name, t);
        fflush(stdout);
        if (!ok)
            failed = 1;
    }
    burrow__aes_set_portable(false);
    return failed;
}
