/* math/rand/v2, pseudo random numbers for simulations, tests and games.
 *
 * Go's math/rand/v2. A Mathrand2Rand draws integers in a range, floats in
 * [0, 1), normally and exponentially distributed floats, permutations and
 * shuffles from a Mathrand2Source, which is anything that hands out 64 random
 * bits at a time. Two sources come with it: Mathrand2PCG, which is small and
 * fast, and Mathrand2ChaCha8, which is stronger and still fast. Given the same
 * seed, both produce exactly the sequence Go's do.
 *
 *     Mathrand2PCG pcg = {1, 2};
 *     Mathrand2Rand r = {mathrand2_pcg_as_source(&pcg)};
 *     Int die = mathrand2_rand_int_n(&r, 6) + 1;
 *
 * The functions without a Rand, mathrand2_int_n and the rest, use a generator
 * seeded from the system that nobody can seed, the same as Go's top level
 * functions. They are safe to call from any number of threads at once. A Rand
 * and a source are not; give each thread its own or put a lock around them.
 *
 * None of this is for keys, tokens or anything else that has to be hard to
 * guess. That is what crypto/rand is for.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package math/rand/v2 */

#ifndef BURROW_MATH_RAND_V2_H
#define BURROW_MATH_RAND_V2_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/func.h"
#include "burrow/iface.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/slice.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ Source */

/* rand.Source: a stream of uniformly distributed 64 bit values. A source is
 * used by one thread at a time unless it says otherwise. */
typedef struct Mathrand2SourceVT {
    const Type *self_type;
    uint64_t (*uint64)(void *self);
} Mathrand2SourceVT;

typedef struct Mathrand2Source {
    const Mathrand2SourceVT *vt;
    void *data;
} Mathrand2Source;

static inline uint64_t mathrand2_source_uint64(Mathrand2Source s) {
    return s.vt->uint64(s.data);
}

/* -------------------------------------------------------------------- Rand */

/* rand.Rand. There is nothing in it but the source, so one can be declared
 * with the source in braces and needs no freeing. */
typedef struct Mathrand2Rand {
    Mathrand2Source src;
} Mathrand2Rand;

/* A Rand from a, drawing from src. Free it with mem_free, or let the arena go.
 * Declaring one, as the example at the top does, is usually simpler. */
BURROW_OWNS(ret) Mathrand2Rand *mathrand2_new(Alloc *a, Mathrand2Source src);

/* A non negative 63 bit integer, 31 bit integer and Int. */
int64_t mathrand2_rand_int64(Mathrand2Rand *r);
int32_t mathrand2_rand_int32(Mathrand2Rand *r);
Int mathrand2_rand_int(Mathrand2Rand *r);

/* A 32 bit, 64 bit and Uint sized value, every bit random. */
uint32_t mathrand2_rand_uint32(Mathrand2Rand *r);
uint64_t mathrand2_rand_uint64(Mathrand2Rand *r);
Uint mathrand2_rand_uint(Mathrand2Rand *r);

/* A value in [0, n). They panic when n is zero, or negative for the signed
 * ones. Go's Lemire rejection is followed draw for draw, so a seeded source
 * gives the same answers here as in Go. */
int64_t mathrand2_rand_int64_n(Mathrand2Rand *r, int64_t n);
int32_t mathrand2_rand_int32_n(Mathrand2Rand *r, int32_t n);
Int mathrand2_rand_int_n(Mathrand2Rand *r, Int n);
uint64_t mathrand2_rand_uint64_n(Mathrand2Rand *r, uint64_t n);
uint32_t mathrand2_rand_uint32_n(Mathrand2Rand *r, uint32_t n);
Uint mathrand2_rand_uint_n(Mathrand2Rand *r, Uint n);

/* Go's generic N for any integer type, which is mostly used with a Duration.
 * The macro picks the signed or unsigned function from the type of n, and the
 * result converts back to it on assignment. */
int64_t burrow__mathrand2_rand_n_signed(Mathrand2Rand *r, int64_t n);
uint64_t burrow__mathrand2_rand_n_unsigned(Mathrand2Rand *r, uint64_t n);
#define BURROW__MATHRAND2_N_FN(n)                                                      \
    _Generic((n),                                                                      \
        unsigned char: burrow__mathrand2_rand_n_unsigned,                              \
        unsigned short: burrow__mathrand2_rand_n_unsigned,                             \
        unsigned int: burrow__mathrand2_rand_n_unsigned,                               \
        unsigned long: burrow__mathrand2_rand_n_unsigned,                              \
        unsigned long long: burrow__mathrand2_rand_n_unsigned,                         \
        default: burrow__mathrand2_rand_n_signed)
#define mathrand2_rand_n(r, n) (BURROW__MATHRAND2_N_FN(n)((r), (n)))

/* A float64 in [0.0, 1.0) and a float32 in [0.0, 1.0). */
double mathrand2_rand_float64(Mathrand2Rand *r);
float mathrand2_rand_float32(Mathrand2Rand *r);

/* A normally distributed float64 with mean 0 and standard deviation 1, and an
 * exponentially distributed one with rate 1. Scale them for other parameters:
 * mathrand2_rand_norm_float64(r) * sd + mean, and
 * mathrand2_rand_exp_float64(r) / rate. */
double mathrand2_rand_norm_float64(Mathrand2Rand *r);
double mathrand2_rand_exp_float64(Mathrand2Rand *r);

/* A permutation of the integers [0, n) as a slice of Int from a. */
BURROW_OWNS(ret) Slice mathrand2_rand_perm(Mathrand2Rand *r, Alloc *a, Int n);

/* Shuffles n elements by calling swap, a Fisher-Yates shuffle with the same
 * swaps Go makes. Panics when n is negative. */
void mathrand2_rand_shuffle(Mathrand2Rand *r, Int n, SwapFunc swap);

/* ------------------------------------------------------------------ PCG */

/* rand.PCG, a 128 bit permuted congruential generator with a DXSM output
 * permutation. The two words are the state and can be set directly:
 * Mathrand2PCG p = {seed1, seed2} is the same as mathrand2_new_pcg. */
typedef struct Mathrand2PCG {
    uint64_t hi;
    uint64_t lo;
} Mathrand2PCG;

/* A PCG from a seeded with seed1 and seed2. */
BURROW_OWNS(ret) Mathrand2PCG *mathrand2_new_pcg(Alloc *a, uint64_t seed1,
                                                 uint64_t seed2);

/* Resets p to what mathrand2_new_pcg(seed1, seed2) gives. */
void mathrand2_pcg_seed(Mathrand2PCG *p, uint64_t seed1, uint64_t seed2);

/* The next value. */
uint64_t mathrand2_pcg_uint64(Mathrand2PCG *p);

/* The state in Go's binary form, "pcg:" and the two words big endian, which
 * mathrand2_pcg_unmarshal_binary and Go's UnmarshalBinary both read. */
BURROW_OWNS(ret) Slice mathrand2_pcg_marshal_binary(Mathrand2PCG *p, Alloc *a,
                                                    Error *err);
BURROW_OWNS(ret) BURROW_BORROWS(ret, b) Slice
mathrand2_pcg_append_binary(Mathrand2PCG *p, Alloc *a, Slice b, Error *err);
BURROW_STATIC(ret) Error mathrand2_pcg_unmarshal_binary(Mathrand2PCG *p, Slice data);

/* p as a Source, for a Rand to draw from. */
Mathrand2Source mathrand2_pcg_as_source(Mathrand2PCG *p);

/* --------------------------------------------------------------- ChaCha8 */

/* rand.ChaCha8, the ChaCha8 stream cipher used as a generator, with the key
 * erasure Go's has: every 1024 bytes the key is replaced by output, so the
 * state after a draw says nothing about the draws before it. The members are
 * the implementation's. */
typedef struct Mathrand2ChaCha8 {
    uint64_t buf[32];
    uint64_t seed[4];
    uint32_t i;
    uint32_t n;
    uint32_t c;
    Byte read_buf[8];
    Int read_len;
} Mathrand2ChaCha8;

/* A ChaCha8 from a seeded with the 32 bytes at seed. */
BURROW_OWNS(ret) Mathrand2ChaCha8 *mathrand2_new_cha_cha8(Alloc *a,
                                                          const Byte seed[32]);

/* Resets c to what mathrand2_new_cha_cha8(seed) gives. It also works on a
 * declared Mathrand2ChaCha8, which is how to have one without an allocator. */
void mathrand2_cha_cha8_seed(Mathrand2ChaCha8 *c, const Byte seed[32]);

/* The next value. */
uint64_t mathrand2_cha_cha8_uint64(Mathrand2ChaCha8 *c);

/* Fills p with random bytes, which is always all of p and never an error. A
 * read that ends part way into a value keeps the rest of it for the next read,
 * so reads of any size give the same bytes as one big one. */
Int mathrand2_cha_cha8_read(Mathrand2ChaCha8 *c, Slice p, Error *err);

/* The state in Go's binary form, which Go's ChaCha8 reads back too. */
BURROW_OWNS(ret) Slice mathrand2_cha_cha8_marshal_binary(Mathrand2ChaCha8 *c, Alloc *a,
                                                         Error *err);
BURROW_OWNS(ret) BURROW_BORROWS(ret, b) Slice
mathrand2_cha_cha8_append_binary(Mathrand2ChaCha8 *c, Alloc *a, Slice b, Error *err);
BURROW_STATIC(ret) Error mathrand2_cha_cha8_unmarshal_binary(Mathrand2ChaCha8 *c,
                                                             Slice data);

/* c as a Source, and as an io.Reader for its Read. */
Mathrand2Source mathrand2_cha_cha8_as_source(Mathrand2ChaCha8 *c);
IoReader mathrand2_cha_cha8_as_io_reader(Mathrand2ChaCha8 *c);

/* -------------------------------------------------------------------- Zipf */

/* rand.Zipf, which draws from a Zipf distribution: P(k) is proportional to
 * (v + k) ** (-s) for k in [0, imax]. */
typedef struct Mathrand2Zipf {
    Mathrand2Rand *r;
    double imax;
    double v;
    double q;
    double s;
    double one_minus_q;
    double one_minus_q_inv;
    double hxm;
    double hx0_minus_hxm;
} Mathrand2Zipf;

/* A Zipf drawing from r with s > 1 and v >= 1, or NULL when either is out of
 * range. r has to outlive it. */
BURROW_OWNS(ret) Mathrand2Zipf *mathrand2_new_zipf(Alloc *a, Mathrand2Rand *r, double s,
                                                   double v, uint64_t imax);

/* A value drawn from the distribution. Panics on a NULL z, as Go does. */
uint64_t mathrand2_zipf_uint64(Mathrand2Zipf *z);

/* ----------------------------------------------------- top level functions */

/* The same as the methods above, from the shared generator seeded by the
 * system. */
int64_t mathrand2_int64(void);
int32_t mathrand2_int32(void);
Int mathrand2_int(void);
uint32_t mathrand2_uint32(void);
uint64_t mathrand2_uint64(void);
Uint mathrand2_uint(void);
int64_t mathrand2_int64_n(int64_t n);
int32_t mathrand2_int32_n(int32_t n);
Int mathrand2_int_n(Int n);
uint64_t mathrand2_uint64_n(uint64_t n);
uint32_t mathrand2_uint32_n(uint32_t n);
Uint mathrand2_uint_n(Uint n);
double mathrand2_float64(void);
float mathrand2_float32(void);
double mathrand2_norm_float64(void);
double mathrand2_exp_float64(void);
BURROW_OWNS(ret) Slice mathrand2_perm(Alloc *a, Int n);
void mathrand2_shuffle(Int n, SwapFunc swap);

/* The shared generator as a Rand, for code that takes one. It has no state
 * and never needs freeing. */
BURROW_STATIC(ret) Mathrand2Rand *burrow__mathrand2_global(void);
#define mathrand2_n(n) (BURROW__MATHRAND2_N_FN(n)(burrow__mathrand2_global(), (n)))

#ifdef __cplusplus
}
#endif

#endif /* BURROW_MATH_RAND_V2_H */
