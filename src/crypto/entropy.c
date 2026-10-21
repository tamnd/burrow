/* Derived from Go's src/crypto/internal/entropy/v1.0.0/entropy.go and
 * sha384.go, and src/crypto/internal/fips140deps/time.
 * Go source: go1.27.1.
 *
 * Package entropy implements a CPU jitter-based SP 800-90B entropy source.
 *
 * In Go it sits outside the FIPS 140-3 module, and so it does here: nothing in
 * this file is in the module's sections.
 *
 * Copyright 2025 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* First, for the amalgamation, which puts a file in the package of the first
 * burrow header it includes. Only the DRBG uses the entropy source, and the
 * DRBG is built on AES. */
#include "burrow/crypto/aes.h"

#include "burrow/atomic.h"
#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/pal.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include "entropy_internal.h"

#include <stdint.h>
#include <string.h>

const char *burrow__entropy_version(void) {
    return "v1.0.0";
}

static Error entropy_error(Str msg) {
    return errors_new(error_allocator(), msg);
}

Error burrow__entropy_seed(Byte seed[48], EntropyScratchBuffer *memory) {
    /* Collect w = 1024 samples, each certified to provide no less than h = 0.5
     * bits of entropy, for a total of hᵢₙ = w × h = 512 bits of entropy, over
     * nᵢₙ = w × n = 8192 bits of input data. */
    Byte samples[1024];
    Error err =
        burrow__entropy_samples(slice_from(samples, 1024, 1024, TYPE_BYTE), memory);
    if (BURROW_FAILED(err)) {
        memset(seed, 0, 48);
        return err;
    }

    /* Use a vetted unkeyed conditioning component, SHA-384, with nw = 384 and
     * nₒᵤₜ = 384. Per the formula in SP 800-90B Section 3.1.5.1.2, the output
     * entropy hₒᵤₜ is:
     *
     *     sage: n_in = 8192
     *     sage: n_out = 384
     *     sage: nw = 384
     *     sage: h_in = 512
     *     sage: P_high = 2^(-h_in)
     *     sage: P_low = (1 - P_high) / (2^n_in - 1)
     *     sage: n = min(n_out, nw)
     *     sage: ψ = 2^(n_in - n) * P_low + P_high
     *     sage: U = 2^(n_in - n) + sqrt(2 * n * 2^(n_in - n) * ln(2))
     *     sage: ω = U * P_low
     *     sage: h_out = -log(max(ψ, ω), 2)
     *     sage: h_out.n()
     *     384.000000000000
     *
     * According to Implementation Guidance D.K, Resolution 19, since
     *
     *   - the conditioning component is vetted,
     *   - hᵢₙ = 512 ≥ nₒᵤₜ + 64 = 448, and
     *   - nₒᵤₜ ≤ security strength of SHA-384 = 384 (per SP 800-107 Rev. 1,
     *     Table 1),
     *
     * we can claim the output has full entropy. */
    burrow__entropy_sha384(seed, samples);
    return BURROW_NO_ERROR;
}

typedef struct EntropySource {
    EntropyScratchBuffer *memory;
    uint32_t lcg_state;
    int64_t previous;
} EntropySource;

static EntropySource entropy_new_source(EntropyScratchBuffer *memory) {
    EntropySource s;
    s.memory = memory;
    s.lcg_state = (uint32_t)pal_clock_high_precision();
    s.previous = pal_clock_high_precision();
    return s;
}

/* touchMemory performs a write to memory at the given index.
 *
 * The memory slice is passed in and may be shared across sources e.g. to avoid
 * the significant (~500µs) cost of zeroing a new allocation on every [Seed]
 * call. */
static void entropy_touch_memory(EntropyScratchBuffer *memory, uint32_t idx) {
    idx = idx / 4 * 4; /* align to 32 bits */
    uint32_t *u32 = &memory->words[idx / 4];
    uint32_t last = burrow__atomic_load_u32(u32);
    (void)burrow__atomic_swap_u32(u32, last + 13);
}

static uint8_t entropy_sample(EntropySource *s) {
    /* Perform a few memory accesses in an unpredictable pattern to expose the
     * next measurement to as much system noise as possible. */
    EntropyScratchBuffer *memory = s->memory;
    uint32_t lcg_state = s->lcg_state;
    if (memory == NULL) /* remove the nil check from the inlined touchMemory calls */
        panic_str(BURROW_S("entropy: nil memory buffer"));
    for (int i = 0; i < 64; i++) {
        lcg_state = 1664525u * lcg_state + 1013904223u;
        /* Discard the lower bits, which tend to fall into short cycles. */
        uint32_t idx = (lcg_state >> 6) & ((1u << 25) - 1);
        entropy_touch_memory(memory, idx);
    }
    s->lcg_state = lcg_state;

    int64_t t = pal_clock_high_precision();
    int64_t sample = (int64_t)((uint64_t)t - (uint64_t)s->previous);
    s->previous = t;

    /* Reduce the symbol space to 256 values, assuming most of the entropy is
     * in the least-significant bits, which represent the highest-resolution
     * timing differences. */
    return (uint8_t)(uint64_t)sample;
}

Error burrow__entropy_samples(Slice samples, EntropyScratchBuffer *memory) {
    if (samples.len < 1024)
        return entropy_error(BURROW_S(
            "entropy: at least 1024 samples are required for startup health tests"));
    EntropySource s = entropy_new_source(memory);
    for (int i = 0; i < 4; i++) {
        /* Warm up the source to avoid any initial bias. */
        (void)entropy_sample(&s);
    }
    uint8_t *p = samples.p;
    for (Int i = 0; i < samples.len; i++)
        p[i] = entropy_sample(&s);
    Error err = burrow__entropy_repetition_count_test(samples);
    if (BURROW_FAILED(err))
        return err;
    err = burrow__entropy_adaptive_proportion_test(samples);
    if (BURROW_FAILED(err))
        return err;
    return BURROW_NO_ERROR;
}

/* RepetitionCountTest implements the repetition count test from SP 800-90B
 * Section 4.4.1. It returns an error if any symbol is repeated C = 41 or more
 * times in a row.
 *
 * This C value is calculated from a target failure probability α = 2⁻²⁰ and a
 * claimed min-entropy per symbol h = 0.5 bits, using the formula in SP 800-90B
 * Section 4.4.1.
 *
 *	sage: α = 2^-20
 *	sage: H = 0.5
 *	sage: 1 + ceil(-log(α, 2) / H)
 *	41 */
Error burrow__entropy_repetition_count_test(Slice samples) {
    const uint8_t *p = samples.p;
    uint8_t x = p[0];
    int count = 1;
    for (Int i = 1; i < samples.len; i++) {
        uint8_t y = p[i];
        if (y == x) {
            count++;
            if (count >= 41)
                return entropy_error(
                    BURROW_S("entropy: repetition count health test failed"));
        } else {
            x = y;
            count = 1;
        }
    }
    return BURROW_NO_ERROR;
}

/* AdaptiveProportionTest implements the adaptive proportion test from SP
 * 800-90B Section 4.4.2. It returns an error if any symbol appears C = 410 or
 * more times in the last W = 512 samples.
 *
 * This C value is calculated from a target failure probability α = 2⁻²⁰, a
 * window size W = 512, and a claimed min-entropy per symbol h = 0.5 bits, using
 * the formula in SP 800-90B Section 4.4.2, equivalent to the Microsoft Excel
 * formula 1+CRITBINOM(W, power(2,(−H)),1−α).
 *
 *	sage: from scipy.stats import binom
 *	sage: α = 2^-20
 *	sage: H = 0.5
 *	sage: W = 512
 *	sage: C = 1 + binom.ppf(1 - α, W, 2**(-H))
 *	sage: ceil(C)
 *	410 */
Error burrow__entropy_adaptive_proportion_test(Slice samples) {
    const uint8_t *p = samples.p;
    int counts[256] = {0};
    for (Int i = 0; i < samples.len; i++) {
        uint8_t x = p[i];
        counts[x]++;
        if (i >= 512)
            counts[p[i - 512]]--;
        if (counts[x] >= 410)
            return entropy_error(
                BURROW_S("entropy: adaptive proportion health test failed"));
    }
    return BURROW_NO_ERROR;
}

/* ------------------------------------------------------------------ SHA-384 */

/* This file includes a SHA-384 implementation to insulate the entropy source
 * from any changes in the FIPS 140-3 module's crypto/internal/fips140/sha512
 * package. We support 1024-byte inputs for the entropy source, and arbitrary
 * length inputs for ACVP testing. */

static const uint64_t entropy_init_state[8] = {
    0xcbbb9d5dc1059ed8ULL, 0x629a292a367cd507ULL, 0x9159015a3070dd17ULL,
    0x152fecd8f70e5939ULL, 0x67332667ffc00b31ULL, 0x8eb44a8768581511ULL,
    0xdb0c2e0d64f98fa7ULL, 0x47b5481dbefa4fa4ULL,
};

static const uint64_t entropy_k[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL,
    0xe9b5dba58189dbbcULL, 0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL,
    0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL, 0xd807aa98a3030242ULL,
    0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL,
    0xc19bf174cf692694ULL, 0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL,
    0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL, 0x2de92c6f592b0275ULL,
    0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL,
    0xbf597fc7beef0ee4ULL, 0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL,
    0x06ca6351e003826fULL, 0x142929670a0e6e70ULL, 0x27b70a8546d22ffcULL,
    0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL,
    0x92722c851482353bULL, 0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL,
    0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL, 0xd192e819d6ef5218ULL,
    0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL,
    0x34b0bcb5e19b48a8ULL, 0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL,
    0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL, 0x748f82ee5defb2fcULL,
    0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL,
    0xc67178f2e372532bULL, 0xca273eceea26619cULL, 0xd186b8c721c0c207ULL,
    0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL, 0x06f067aa72176fbaULL,
    0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL,
    0x431d67c49c100d4cULL, 0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL,
    0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL,
};

static uint64_t entropy_rotr(uint64_t x, unsigned n) {
    return (x >> n) | (x << (64 - n));
}

static uint64_t entropy_be_uint64(const Byte *b) {
    return (uint64_t)b[7] | (uint64_t)b[6] << 8 | (uint64_t)b[5] << 16 |
           (uint64_t)b[4] << 24 | (uint64_t)b[3] << 32 | (uint64_t)b[2] << 40 |
           (uint64_t)b[1] << 48 | (uint64_t)b[0] << 56;
}

static void entropy_be_put_uint64(Byte *b, uint64_t v) {
    b[0] = (Byte)(v >> 56);
    b[1] = (Byte)(v >> 48);
    b[2] = (Byte)(v >> 40);
    b[3] = (Byte)(v >> 32);
    b[4] = (Byte)(v >> 24);
    b[5] = (Byte)(v >> 16);
    b[6] = (Byte)(v >> 8);
    b[7] = (Byte)v;
}

static void entropy_sha384_block(uint64_t dh[8], const Byte p[128]) {
    uint64_t w[80];
    for (int i = 0; i < 80; i++) {
        if (i < 16) {
            w[i] = entropy_be_uint64(p + i * 8);
        } else {
            uint64_t v1 = w[i - 2];
            uint64_t t1 = entropy_rotr(v1, 19) ^ entropy_rotr(v1, 61) ^ (v1 >> 6);
            uint64_t v2 = w[i - 15];
            uint64_t t2 = entropy_rotr(v2, 1) ^ entropy_rotr(v2, 8) ^ (v2 >> 7);

            w[i] = t1 + w[i - 7] + t2 + w[i - 16];
        }
    }

    uint64_t a = dh[0], b = dh[1], c = dh[2], d = dh[3];
    uint64_t e = dh[4], f = dh[5], g = dh[6], h = dh[7];

    for (int i = 0; i < 80; i++) {
        uint64_t t1 =
            h + (entropy_rotr(e, 14) ^ entropy_rotr(e, 18) ^ entropy_rotr(e, 41)) +
            ((e & f) ^ (~e & g)) + entropy_k[i] + w[i];
        uint64_t t2 =
            (entropy_rotr(a, 28) ^ entropy_rotr(a, 34) ^ entropy_rotr(a, 39)) +
            ((a & b) ^ (a & c) ^ (b & c));

        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    dh[0] += a;
    dh[1] += b;
    dh[2] += c;
    dh[3] += d;
    dh[4] += e;
    dh[5] += f;
    dh[6] += g;
    dh[7] += h;
}

static void entropy_digest_bytes(Byte digest[48], const uint64_t h[8]) {
    for (int i = 0; i < 6; i++)
        entropy_be_put_uint64(digest + 8 * i, h[i]);
}

void burrow__entropy_sha384(Byte digest[48], const Byte p[1024]) {
    uint64_t h[8];
    memcpy(h, entropy_init_state, sizeof h);

    for (int i = 0; i < 8; i++)
        entropy_sha384_block(h, p + 128 * i);

    Byte padlen[128] = {0};
    padlen[0] = 0x80;
    entropy_be_put_uint64(padlen + 112 + 8, 1024 * 8);
    entropy_sha384_block(h, padlen);

    entropy_digest_bytes(digest, h);
}

void burrow__entropy_testing_only_sha384(Byte digest[48], Slice p) {
    const Byte *b = p.p;
    if (p.len == 1024) {
        burrow__entropy_sha384(digest, b);
        return;
    }

    uint64_t h[8];
    memcpy(h, entropy_init_state, sizeof h);
    uint64_t bit_len = (uint64_t)p.len * 8;
    Int n = p.len;

    /* Process full 128-byte blocks. */
    while (n >= 128) {
        entropy_sha384_block(h, b);
        b += 128;
        n -= 128;
    }

    /* Process final block and padding. */
    Byte final_block[128] = {0};
    if (n > 0)
        memcpy(final_block, b, (size_t)n);
    final_block[n] = 0x80;
    if (n >= 112) {
        entropy_sha384_block(h, final_block);
        memset(final_block, 0, sizeof final_block);
    }
    entropy_be_put_uint64(final_block + 112 + 8, bit_len);
    entropy_sha384_block(h, final_block);

    entropy_digest_bytes(digest, h);
}
